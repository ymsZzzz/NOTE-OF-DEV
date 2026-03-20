#include "tx_api.h"
#include <stdio.h>
#include <string.h>

/* ============================================
 *  第一部分：系统配置与宏定义
 * ============================================ */

/* 时间转换宏：毫秒转 Tick */
#define MS_TO_TICKS(ms)         ((ms) * TX_TIMER_TICKS_PER_SECOND / 1000)
#define TICKS_TO_MS(ticks)      ((ticks) * 1000 / TX_TIMER_TICKS_PER_SECOND)

/* 线程优先级定义 */
#define PRIO_SENSOR_ACQ         10      /* 传感器采集 - 最高优先级 */
#define PRIO_DATA_PROC          11      /* 数据处理 - 中等优先级，使用抢占阈值优化 */
#define PRIO_COMM_SEND          12      /* 通信发送 - 较低优先级 */
#define PRIO_SYSTEM_MON         15      /* 系统监控 - 最低优先级 */

/* 抢占阈值优化：数据处理线程优先级11，但阈值设为10，
 * 这样当它运行时，传感器采集线程(10)无法抢占它，避免I2C总线冲突 */
#define PREEMPT_THRESHOLD_DATA  10

/* 事件标志位定义（用于事件链模式） */
#define EVENT_SENSOR_READY      0x00000001  /* 传感器数据就绪 */
#define EVENT_COMM_COMPLETE     0x00000002  /* 通信完成 */
#define EVENT_TIMEOUT           0x00000004  /* 定时器超时 */
#define EVENT_ERROR_FLAG        0x00000008  /* 错误标志 */

/* 返回值检查宏 - 生产环境建议改为日志记录 */
#define TX_CHECK(status)        do { \
                                    if ((status) != TX_SUCCESS) { \
                                        while(1); /* 断点或复位 */ \
                                    } \
                                } while(0)

/* ============================================
 *  第二部分：数据结构定义
 * ============================================ */

/* 传感器数据结构 - 恰好放入一个内存块 */
typedef struct {
    uint32_t timestamp;
    uint16_t sensor_id;
    float    temperature;
    float    humidity;
    float    pressure;
    uint16_t checksum;
} sensor_data_t;

/* 通信数据包结构 */
typedef struct {
    uint8_t  header[4];
    uint8_t  payload[sizeof(sensor_data_t)];
    uint8_t  footer[2];
} comm_packet_t;

/* ============================================
 *  第三部分：全局资源声明（静态分配，精确控制）
 * ============================================ */

/* 线程控制块 - 必须静态分配 */
TX_THREAD       thread_sensor_acq_cb;
TX_THREAD       thread_data_proc_cb;
TX_THREAD       thread_comm_send_cb;
TX_THREAD       thread_system_mon_cb;

/* 同步与通信对象控制块 */
TX_QUEUE        queue_sensor_data;          /* 传感器数据队列（传递指针） */
TX_QUEUE        queue_comm_packet;          /* 通信数据队列 */
TX_SEMAPHORE    sem_i2c_bus;                /* I2C总线互斥（演示用，实际可用抢占阈值优化替代） */
TX_MUTEX        mutex_sensor_config;        /* 传感器配置互斥 */
TX_EVENT_FLAGS_GROUP event_group_main;      /* 主事件组 - 事件链核心 */
TX_TIMER        timer_periodic;             /* 周期定时器 - 警告：回调中严禁阻塞！ */
TX_TIMER        timer_watchdog;             /* 看门狗定时器 */

/* 内存池对象 */
TX_BYTE_POOL    byte_pool_main;             /* 主字节池 - 用于线程栈等 */
TX_BLOCK_POOL   block_pool_sensor;          /* 传感器数据块池 - 零拷贝核心 */

/* 内存区域 - 实际项目中可用链接器导出的未用RAM */
#define MAIN_POOL_SIZE          (32 * 1024)     /* 32KB 主内存池 */
#define SENSOR_BLOCK_SIZE       sizeof(sensor_data_t)
#define SENSOR_BLOCK_COUNT      16                /* 最多16个待处理数据 */

static UCHAR    memory_area[MAIN_POOL_SIZE];

/* 队列缓冲区 - 存储指针 */
static void*    queue_sensor_buffer[SENSOR_BLOCK_COUNT];
static void*    queue_comm_buffer[8];

/* 统计计数器 */
volatile ULONG  stat_sensor_count = 0;
volatile ULONG  stat_drop_count = 0;
volatile ULONG  stat_timer_ticks = 0;

/* ============================================
 *  第四部分：辅助函数
 * ============================================ */

/* 模拟硬件操作 */
static void hal_i2c_init(void) { /* ... */ }
static void hal_i2c_read(uint8_t addr, uint8_t* buf, uint16_t len) { 
    /* 模拟读取耗时 */
    tx_thread_sleep(MS_TO_TICKS(2));
}
static void hal_uart_send(uint8_t* data, uint16_t len) {
    /* 模拟发送耗时 */
    tx_thread_sleep(MS_TO_TICKS(5));
}

/* CRC校验计算 */
static uint16_t calculate_checksum(sensor_data_t* data) {
    uint16_t sum = 0;
    uint8_t* ptr = (uint8_t*)data;
    for (int i = 0; i < sizeof(sensor_data_t) - 2; i++) {
        sum += ptr[i];
    }
    return sum;
}

/* ============================================
 *  第五部分：线程实现
 * ============================================ */

/* 线程1：传感器数据采集（最高优先级） */
void thread_sensor_acq_entry(ULONG thread_input)
{
    UINT status;
    sensor_data_t* data_ptr;
    
    (void)thread_input;
    
    printf("[SENSOR] 采集线程启动，优先级 %d\n", PRIO_SENSOR_ACQ);
    
    while (1) {
        /* 从块池申请内存 - O(1) 时间，零拷贝架构核心 */
        status = tx_block_allocate(&block_pool_sensor, (VOID**)&data_ptr, TX_NO_WAIT);
        
        if (status == TX_SUCCESS) {
            /* 模拟传感器读取 */
            data_ptr->timestamp = tx_time_get();
            data_ptr->sensor_id = 0x01;
            data_ptr->temperature = 25.0f + (float)(tx_time_get() % 100) / 10.0f;
            data_ptr->humidity = 50.0f + (float)(tx_time_get() % 50) / 10.0f;
            data_ptr->pressure = 1013.25f;
            data_ptr->checksum = calculate_checksum(data_ptr);
            
            /* 发送到处理队列 - 只传递指针，不拷贝数据 */
            status = tx_queue_send(&queue_sensor_data, &data_ptr, TX_NO_WAIT);
            
            if (status == TX_SUCCESS) {
                stat_sensor_count++;
                /* 设置事件标志，通知数据处理线程 - 事件链模式 */
                tx_event_flags_set(&event_group_main, EVENT_SENSOR_READY, TX_OR);
            } else {
                /* 队列满，释放内存块，统计丢包 */
                tx_block_release(data_ptr);
                stat_drop_count++;
            }
        } else {
            /* 块池耗尽，统计丢包 */
            stat_drop_count++;
        }
        
        /* 100ms 采集周期 */
        tx_thread_sleep(MS_TO_TICKS(100));
    }
}

/* 线程2：数据处理（使用抢占阈值优化） */
void thread_data_proc_entry(ULONG thread_input)
{
    UINT status;
    sensor_data_t* data_ptr;
    sensor_data_t local_copy;
    comm_packet_t* packet_ptr;
    ULONG actual_flags;
    
    (void)thread_input;
    
    printf("[PROC] 数据处理线程启动，优先级 %d，抢占阈值 %d\n", 
           PRIO_DATA_PROC, PREEMPT_THRESHOLD_DATA);
    
    while (1) {
        /* 事件链模式：等待多个事件源 */
        status = tx_event_flags_get(&event_group_main,
                                    EVENT_SENSOR_READY | EVENT_TIMEOUT,
                                    TX_OR_CLEAR,  /* 读取后清除 */
                                    &actual_flags,
                                    MS_TO_TICKS(500));  /* 500ms 超时 */
        
        if (status == TX_SUCCESS) {
            if (actual_flags & EVENT_SENSOR_READY) {
                /* 处理所有待处理的数据 */
                while (tx_queue_receive(&queue_sensor_data, &data_ptr, TX_NO_WAIT) == TX_SUCCESS) {
                    
                    /* 验证校验和 */
                    if (data_ptr->checksum == calculate_checksum(data_ptr)) {
                        /* 复制到本地（因为要在块释放前处理） */
                        memcpy(&local_copy, data_ptr, sizeof(sensor_data_t));
                        
                        /* 立即释放内存块 - 零拷贝架构：尽快释放资源 */
                        tx_block_release(data_ptr);
                        
                        /* 申请通信包内存 */
                        status = tx_byte_allocate(&byte_pool_main, (VOID**)&packet_ptr, 
                                                  sizeof(comm_packet_t), TX_NO_WAIT);
                        if (status == TX_SUCCESS) {
                            /* 打包数据 */
                            packet_ptr->header[0] = 0xAA;
                            packet_ptr->header[1] = 0x55;
                            memcpy(packet_ptr->payload, &local_copy, sizeof(sensor_data_t));
                            packet_ptr->footer[0] = 0x0D;
                            packet_ptr->footer[1] = 0x0A;
                            
                            /* 发送到通信队列 */
                            tx_queue_send(&queue_comm_packet, &packet_ptr, TX_NO_WAIT);
                            tx_event_flags_set(&event_group_main, EVENT_COMM_COMPLETE, TX_OR);
                        }
                    } else {
                        /* 校验失败，丢弃 */
                        tx_block_release(data_ptr);
                    }
                }
            }
            
            if (actual_flags & EVENT_TIMEOUT) {
                printf("[PROC] 处理超时，检查系统状态\n");
            }
        }
    }
}

/* 线程3：通信发送 */
void thread_comm_send_entry(ULONG thread_input)
{
    UINT status;
    comm_packet_t* packet_ptr;
    ULONG actual_flags;
    
    (void)thread_input;
    
    printf("[COMM] 通信线程启动\n");
    
    while (1) {
        /* 等待通信数据就绪事件 */
        status = tx_event_flags_get(&event_group_main,
                                    EVENT_COMM_COMPLETE,
                                    TX_OR_CLEAR,
                                    &actual_flags,
                                    TX_WAIT_FOREVER);
        
        if (status == TX_SUCCESS) {
            /* 处理所有待发送数据 */
            while (tx_queue_receive(&queue_comm_packet, &packet_ptr, TX_NO_WAIT) == TX_SUCCESS) {
                /* 模拟发送 */
                hal_uart_send((uint8_t*)packet_ptr, sizeof(comm_packet_t));
                
                /* 释放内存回字节池 */
                tx_byte_release(packet_ptr);
            }
        }
    }
}

/* 线程4：系统监控（最低优先级） */
void thread_system_mon_entry(ULONG thread_input)
{
    TX_THREAD* thread_ptr;
    CHAR* name;
    UINT state;
    ULONG run_count;
    UINT priority;
    UINT preempt_threshold;
    ULONG time_slice;
    TX_QUEUE* next_queue;
    TX_SEMAPHORE* next_semaphore;
    ULONG available_bytes;
    ULONG fragments;
    
    (void)thread_input;
    
    printf("[MON] 监控线程启动\n");
    
    while (1) {
        tx_thread_sleep(MS_TO_TICKS(5000));  /* 每5秒报告一次 */
        
        printf("\n========== 系统状态报告 ==========\n");
        printf("传感器采集数: %lu, 丢包数: %lu\n", stat_sensor_count, stat_drop_count);
        printf("定时器Tick: %lu\n", stat_timer_ticks);
        
        /* 检查内存池状态 */
        tx_byte_pool_info_get(&byte_pool_main, &name, &available_bytes, 
                              &fragments, &next_queue, &next_semaphore);
        printf("内存池: 可用 %lu 字节, 碎片 %lu\n", available_bytes, fragments);
        
        /* 检查块池状态 */
        ULONG total_blocks, free_blocks;
        tx_block_pool_info_get(&block_pool_sensor, &name, &total_blocks, 
                               &free_blocks, &next_queue);
        printf("传感器块池: 总计 %lu, 空闲 %lu\n", total_blocks, free_blocks);
        
        printf("==================================\n\n");
    }
}

/* ============================================
 *  第六部分：定时器回调（绝对禁止阻塞！）
 * ============================================ */

/* 周期定时器回调 - 在中断上下文执行！ */
void timer_periodic_callback(ULONG timer_input)
{
    (void)timer_input;
    
    /* 绝对安全操作：只修改标志位和计数器 */
    stat_timer_ticks++;
    
    /* 可以设置事件标志（非阻塞） */
    if ((stat_timer_ticks % 10) == 0) {
        /* 每10个tick触发一次超时检查 */
        tx_event_flags_set(&event_group_main, EVENT_TIMEOUT, TX_OR);
    }
    
    /* 严禁调用以下API：
     * - tx_queue_receive(..., TX_WAIT_FOREVER) 
     * - tx_semaphore_get(..., TX_WAIT_FOREVER)
     * - tx_thread_sleep(...)
     * - tx_byte_allocate(..., TX_WAIT_FOREVER)
     * 这些都会导致系统时钟卡死！
     */
}

/* 看门狗定时器回调 */
void timer_watchdog_callback(ULONG timer_input)
{
    (void)timer_input;
    /* 检查各线程健康状态，必要时复位 */
}

/* ============================================
 *  第七部分：系统初始化（ThreadX 核心差异）
 * ============================================ */

/* 这是 ThreadX 强制回调的初始化函数！
 * 所有线程、队列、内存池必须在此创建。
 * 注意：此函数返回后，ThreadX 立即开始调度，永不返回 main() */
void tx_application_define(void *first_unused_memory)
{
    UINT status;
    void* stack_ptr;
    void* pool_start;
    ULONG pool_size;
    
    printf("[INIT] tx_application_define 开始执行\n");
    
    /* 方案A：使用链接器提供的未用内存（推荐用于生产环境） */
    /* pool_start = first_unused_memory; */
    /* pool_size = 计算剩余RAM大小; */
    
    /* 方案B：使用预定义的全局数组（演示用） */
    pool_start = memory_area;
    pool_size = MAIN_POOL_SIZE;
    
    /* 1. 创建主字节池 - "一池打天下"模式 */
    status = tx_byte_pool_create(&byte_pool_main, "Main Pool", 
                                 pool_start, pool_size);
    TX_CHECK(status);
    printf("[INIT] 主字节池创建成功，大小 %lu 字节\n", pool_size);
    
    /* 2. 从字节池分配内存，创建传感器数据块池 - 零拷贝架构基础 */
    void* block_pool_memory;
    ULONG block_pool_size = SENSOR_BLOCK_SIZE * SENSOR_BLOCK_COUNT;
    
    status = tx_byte_allocate(&byte_pool_main, &block_pool_memory, 
                              block_pool_size, TX_NO_WAIT);
    TX_CHECK(status);
    
    status = tx_block_pool_create(&block_pool_sensor, "Sensor Pool",
                                  SENSOR_BLOCK_SIZE, block_pool_memory,
                                  block_pool_size);
    TX_CHECK(status);
    printf("[INIT] 传感器块池创建成功，%lu 个块，每块 %lu 字节\n", 
           SENSOR_BLOCK_COUNT, (ULONG)SENSOR_BLOCK_SIZE);
    
    /* 3. 创建队列 */
    status = tx_queue_create(&queue_sensor_data, "Sensor Queue",
                             sizeof(void*)/sizeof(ULONG),  /* 每个条目大小（ULONG数） */
                             queue_sensor_buffer,
                             sizeof(queue_sensor_buffer));
    TX_CHECK(status);
    
    status = tx_queue_create(&queue_comm_packet, "Comm Queue",
                             sizeof(void*)/sizeof(ULONG),
                             queue_comm_buffer,
                             sizeof(queue_comm_buffer));
    TX_CHECK(status);
    printf("[INIT] 队列创建成功\n");
    
    /* 4. 创建同步对象 */
    status = tx_semaphore_create(&sem_i2c_bus, "I2C Bus", 1);
    TX_CHECK(status);
    
    status = tx_mutex_create(&mutex_sensor_config, "Sensor Config", TX_NO_INHERIT);
    TX_CHECK(status);
    
    status = tx_event_flags_create(&event_group_main, "Main Events");
    TX_CHECK(status);
    printf("[INIT] 同步对象创建成功\n");
    
    /* 5. 创建线程 - 所有栈从字节池分配 */
    
    /* 线程1：传感器采集（优先级10） */
    status = tx_byte_allocate(&byte_pool_main, &stack_ptr, 2048, TX_NO_WAIT);
    TX_CHECK(status);
    status = tx_thread_create(&thread_sensor_acq_cb, "Sensor Acq",
                              thread_sensor_acq_entry, 0,
                              stack_ptr, 2048,
                              PRIO_SENSOR_ACQ, PRIO_SENSOR_ACQ,
                              TX_NO_TIME_SLICE, TX_AUTO_START);
    TX_CHECK(status);
    
    /* 线程2：数据处理（优先级11，抢占阈值10 - 关键优化！） */
    status = tx_byte_allocate(&byte_pool_main, &stack_ptr, 2048, TX_NO_WAIT);
    TX_CHECK(status);
    status = tx_thread_create(&thread_data_proc_cb, "Data Proc",
                              thread_data_proc_entry, 0,
                              stack_ptr, 2048,
                              PRIO_DATA_PROC, PREEMPT_THRESHOLD_DATA,  /* 抢占阈值优化 */
                              TX_NO_TIME_SLICE, TX_AUTO_START);
    TX_CHECK(status);
    printf("[INIT] 数据处理线程创建，优先级 %d，抢占阈值 %d\n", 
           PRIO_DATA_PROC, PREEMPT_THRESHOLD_DATA);
    
    /* 线程3：通信发送（优先级12） */
    status = tx_byte_allocate(&byte_pool_main, &stack_ptr, 2048, TX_NO_WAIT);
    TX_CHECK(status);
    status = tx_thread_create(&thread_comm_send_cb, "Comm Send",
                              thread_comm_send_entry, 0,
                              stack_ptr, 2048,
                              PRIO_COMM_SEND, PRIO_COMM_SEND,
                              TX_NO_TIME_SLICE, TX_AUTO_START);
    TX_CHECK(status);
    
    /* 线程4：系统监控（优先级15，最低） */
    status = tx_byte_allocate(&byte_pool_main, &stack_ptr, 1024, TX_NO_WAIT);
    TX_CHECK(status);
    status = tx_thread_create(&thread_system_mon_cb, "System Mon",
                              thread_system_mon_entry, 0,
                              stack_ptr, 1024,
                              PRIO_SYSTEM_MON, PRIO_SYSTEM_MON,
                              TX_NO_TIME_SLICE, TX_AUTO_START);
    TX_CHECK(status);
    printf("[INIT] 所有线程创建成功\n");
    
    /* 6. 创建定时器 - 绝对禁止在回调中使用阻塞API！ */
    status = tx_timer_create(&timer_periodic, "Periodic",
                             timer_periodic_callback, 0,
                             MS_TO_TICKS(100), MS_TO_TICKS(100),  /* 初始延迟100ms，周期100ms */
                             TX_AUTO_ACTIVATE);
    TX_CHECK(status);
    
    status = tx_timer_create(&timer_watchdog, "Watchdog",
                             timer_watchdog_callback, 0,
                             MS_TO_TICKS(1000), MS_TO_TICKS(1000),
                             TX_AUTO_ACTIVATE);
    TX_CHECK(status);
    printf("[INIT] 定时器创建成功（警告：回调中严禁阻塞！）\n");
    
    printf("[INIT] 系统初始化完成，即将启动调度...\n");
    
    /* 函数返回后，ThreadX 立即开始调度，此函数永不返回 */
}

/* ============================================
 *  第八部分：程序入口（与 FreeRTOS 完全不同！）
 * ============================================ */

int main(void)
{
    /* 硬件初始化（此时中断未开启，可安全操作硬件） */
    hal_i2c_init();
    printf("[MAIN] 硬件初始化完成\n");
    
    /* 调用 tx_kernel_enter 后，控制权完全交给 ThreadX
     * 内核准备好后，会自动调用 tx_application_define()
     * 此函数永不返回！ */
    tx_kernel_enter();
    
    /* 以下代码永远不会执行！ */
    printf("这行永远不会打印\n");
    
    return 0;
}

/* ============================================
 *  附录：TraceX 启用配置（可选）
 * ============================================ */

#ifdef TX_ENABLE_EVENT_TRACE

/* 声明 Trace Buffer - 通常需要 20-50KB */
static UCHAR trace_buffer[32768];

void trace_init(void)
{
    /* 在 tx_application_define 最开始调用 */
    tx_trace_enable(trace_buffer, sizeof(trace_buffer), 32);
}

/* 运行后，通过 J-Link 读取 trace_buffer，保存为 .trx 文件
 * 用 TraceX 工具打开，可图形化分析任务切换、中断、队列操作等 */
#endif