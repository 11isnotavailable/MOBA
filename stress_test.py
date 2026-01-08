import asyncio
import time
import struct
import random

# --- 协议配置 (需与 protocol.h 一致) ---
# GamePacket 包含 21 个 int (21 * 4 = 84 bytes)
GAME_PACKET_FMT = "<21i"
TYPE_MOVE = 1

async def simulate_client(client_id, server_ip, server_port, results):
    """模拟单个玩家的行为"""
    writer = None
    try:
        # 1. 建立连接
        start_conn = time.perf_counter()
        # 设置超时，防止死等
        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(server_ip, server_port), timeout=5.0
        )
        results['connections_success'] += 1
        
        # 记录连接延迟 (单位: 秒)
        conn_latency = time.perf_counter() - start_conn
        results['latencies'].append(conn_latency)

        # 2. 模拟持续操作 (移动 5 次)
        for _ in range(5):
            packet_data = [0] * 21
            packet_data[0] = TYPE_MOVE
            packet_data[1] = client_id # 模拟 ID
            packet_data[2] = random.choice([-1, 0, 1]) # dx
            packet_data[3] = random.choice([-1, 0, 1]) # dy
            
            payload = struct.pack(GAME_PACKET_FMT, *packet_data)
            
            writer.write(payload)
            await writer.drain()
            results['packets_sent'] += 1
            
            # 模拟心跳间隔 (100ms)
            await asyncio.sleep(0.1) 

    except Exception as e:
        results['errors'] += 1
    finally:
        if writer:
            writer.close()
            try:
                await writer.wait_closed()
            except:
                pass

async def run_stress_test(server_ip, server_port, total_users, batch_size=50):
    """
    分批运行压测
    batch_size: 每批启动的连接数，建议设为 50-100，避免瞬间冲死服务器 backlog
    """
    results = {
        'connections_success': 0,
        'packets_sent': 0,
        'errors': 0,
        'latencies': []
    }

    print(f"[*] 启动压测: 目标 {server_ip}:{server_port}")
    print(f"[*] 总计划连接: {total_users}, 每批启动: {batch_size}")
    start_time = time.perf_counter()

    # 分批创建任务
    all_tasks = []
    for i in range(0, total_users, batch_size):
        batch = [simulate_client(j, server_ip, server_port, results) 
                 for j in range(i, min(i + batch_size, total_users))]
        all_tasks.extend(batch)
        # 每批启动后微小停顿，给内核 accept 留喘息时间
        await asyncio.sleep(0.05)

    await asyncio.gather(*all_tasks)

    total_time = time.perf_counter() - start_time
    
    # --- 输出统计结果 ---
    print("\n" + "="*40)
    print("           压力测试报告")
    print("="*40)
    print(f"测试总耗时:   {total_time:.2f} s")
    print(f"并发连接数:   {total_users}")
    print(f"连接成功数:   {results['connections_success']}")
    print(f"数据发送量:   {results['packets_sent']} packets")
    print(f"异常失败数:   {results['errors']}")
    
    if results['latencies']:
        avg_lat = (sum(results['latencies']) / len(results['latencies'])) * 1000
        print(f"平均连接延迟: {avg_lat:.2f} ms")
        print(f"最小连接延迟: {min(results['latencies'])*1000:.2f} ms")
        print(f"最大连接延迟: {max(results['latencies'])*1000:.2f} ms")
    
    success_rate = (results['connections_success'] / total_users) * 100
    print(f"连接成功率:   {success_rate:.1f}%")
    print("="*40)

if __name__ == "__main__":
    SERVER_IP = "127.0.0.1"
    SERVER_PORT = 8888
    
    # 第一次测试建议先用 500
    USER_COUNT = 5000
    
    asyncio.run(run_stress_test(SERVER_IP, SERVER_PORT, USER_COUNT))