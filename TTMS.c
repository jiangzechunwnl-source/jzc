/*
 * TTMS.c — 剧院票务管理系统主程序入口
 * 功能：设置标准输出无缓冲后进入主菜单 Main_Menu()
 * 说明：控制台版入口；数据存储于 data.txt，由持久层统一读写
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <io.h>
#include "./src/View/Main_Menu.h"

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	Main_Menu();
	return EXIT_SUCCESS;
}
