//=====================================================================
//
// test.cpp - kcp 测试用例
//
// 说明：
// gcc test.cpp -o test -lstdc++
//
//=====================================================================

#include <stdio.h>
#include <stdlib.h>

#include "test.h"
#include "kcpp.h"

using namespace stone;
// 模拟网络
LatencySimulator *vnet;

// 模拟网络：模拟发送一个 udp包
int udp_output(const char *buf, int len, Kcpp *kcpp, void *user)
{
	union { int id; void *ptr; } parameter;
	parameter.ptr = user;
	vnet->send(parameter.id, buf, len);
	return 0;
}

// 测试用例
void test(int mode)
{
	// 创建模拟网络：丢包率10%，Rtt 60ms~125ms
	vnet = new LatencySimulator(10, 60, 125);
	
	// 创建两个端点的 kcp对象，第一个参数 conv是会话编号，同一个会话需要相同
	// 最后一个是 user参数，用来传递标识
	Kcpp kcpp1(0x11223344, (void*)0);
	Kcpp kcpp2(0x11223344, (void*)1);

	// 设置kcp的下层输出，这里为 udp_output，模拟udp网络输出函数
	kcpp1.set_output(udp_output);
	kcpp2.set_output(udp_output);

	uint32_t current = iclock();
	uint32_t slap = current + 20;
	uint32_t index = 0;
	uint32_t next = 0;
	int64_t sumrtt = 0;
	int count = 0;
	int maxrtt = 0;

	// 配置窗口大小：平均延迟200ms，每20ms发送一个包，
	// 而考虑到丢包重发，设置最大收发窗口为128
	kcpp1.set_wndsize(128, 128);
	kcpp2.set_wndsize(128, 128);

	// 判断测试用例的模式
	if (mode == 0) {
		// 默认模式
		kcpp1.no_delay(0, 10, 0, false);
		kcpp2.no_delay(0, 10, 0, false);

	}
	else if (mode == 1) {
		// 普通模式，关闭流控等
		kcpp1.no_delay(0, 10, 0, true);
		kcpp2.no_delay(0, 10, 0, true);
	}	else {
		// 启动快速模式
		// 第二个参数 nodelay-启用以后若干常规加速将启动
		// 第三个参数 interval为内部处理时钟，默认设置为 10ms
		// 第四个参数 resend为快速重传指标，设置为2
		// 第五个参数 为是否禁用常规流控，这里禁止
		kcpp1.no_delay(2, 10, 2, true);
		kcpp2.no_delay(2, 10, 2, true);
		kcpp1.set_minrto(10);
		kcpp1.set_fastresend(1);
	}


	char buffer[2000];
	int hr;

	uint32_t ts1 = iclock();

	while (1) {
		isleep(1);
		current = iclock();
		kcpp1.update(iclock());
		kcpp2.update(iclock());

		// 每隔 20ms，kcp1发送数据
		for (; current >= slap; slap += 20) {
			((uint32_t*)buffer)[0] = index++;
			((uint32_t*)buffer)[1] = current;

			// 发送上层协议包
			kcpp1.send(buffer, 8);
		}

		// 处理虚拟网络：检测是否有udp包从p1->p2
		while (1) {
			hr = vnet->recv(1, buffer, 2000);
			if (hr < 0) break;
			// 如果 p2收到udp，则作为下层协议输入到kcp2
			kcpp2.input(buffer, hr);
		}

		// 处理虚拟网络：检测是否有udp包从p2->p1
		while (1) {
			hr = vnet->recv(0, buffer, 2000);
			if (hr < 0) break;
			// 如果 p1收到udp，则作为下层协议输入到kcp1
			kcpp1.input(buffer, hr);
		}

		// kcp2接收到任何包都返回回去
		while (1) {
			hr =kcpp2.recv(buffer, 10);

			// 没有收到包就退出
			if (hr < 0) break;
			// 如果收到包就回射
			kcpp2.send(buffer, hr);
		}

		// kcp1收到kcp2的回射数据
		while (1) {
			hr =kcpp1.recv(buffer, 10);

			// 没有收到包就退出
			if (hr < 0) break;
			uint32_t sn = *(uint32_t*)(buffer + 0);
			uint32_t ts = *(uint32_t*)(buffer + 4);
			uint32_t rtt = current - ts;
			
			if (sn != next) {
				// 如果收到的包不连续
				printf("ERROR sn %d<->%d\n", (int)count, (int)next);
				return;
			}

			next++;
			sumrtt += rtt;
			count++;
			if (rtt > (uint32_t)maxrtt) maxrtt = rtt;

			printf("[RECV] mode=%d sn=%d rtt=%d\n", mode, (int)sn, (int)rtt);
		}
		if (next > 1000) break;
	}

	ts1 = iclock() - ts1;

	const char *names[3] = { "default", "normal", "fast" };
	printf("%s mode result (%dms):\n", names[mode], (int)ts1);
	printf("avgrtt=%d maxrtt=%d tx=%d\n", (int)(sumrtt / count), (int)maxrtt, (int)vnet->tx1);
	printf("press enter to next ...\n");
	char ch; scanf("%c", &ch);
}

int main()
{
	test(0);	// 默认模式，类似 TCP：正常模式，无快速重传，常规流控
	test(1);	// 普通模式，关闭流控等
	test(2);	// 快速模式，所有开关都打开，且关闭流控
	return 0;
}

/*
default mode result (20917ms):
avgrtt=740 maxrtt=1507

normal mode result (20131ms):
avgrtt=156 maxrtt=571

fast mode result (20207ms):
avgrtt=138 maxrtt=392
*/


