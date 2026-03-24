/*
 * init_data.c — 初始化数据文件工具
 * 功能：覆盖写入 data.txt，包含默认演出厅、空业务段、默认管理员（admin/admin）
 * 使用：由 init_data.bat 编译运行；慎用，会清空现有 data.txt
 */
#include <stdio.h>

int main(void) {
    FILE *fp = fopen("data.txt", "w");  /* 以覆盖写方式创建/清空 data.txt */
    if (!fp) {
        printf("Cannot create data.txt\n");
        return 1;
    }

    /* [Studio] 段：每行格式 id|名称|行数|列数|已添加座位数 */
    fprintf(fp, "[Studio]\n");
    fprintf(fp, "1|cnm|10|10|0\n");  /* 演出厅 1：cnm，10 行 10 列，0 个座位（需在程序中初始化） */
    fprintf(fp, "2|scs|10|10|0\n");  /* 演出厅 2：scs */
    fprintf(fp, "3|asd|10|10|0\n");  /* 演出厅 3：asd */

    /* [Seat] 段：初始为空，座位在程序中通过演出厅的 [A] 初始化或 [B] 添加 */
    fprintf(fp, "\n[Seat]\n");

    /* [Play] 段：初始为空，剧目在程序中添加 */
    fprintf(fp, "\n[Play]\n");

    /* [Schedule] 段：初始为空，排期在剧目管理中添加 */
    fprintf(fp, "\n[Schedule]\n");

    /* [Ticket] 段：初始为空，新增排期时自动创建 */
    fprintf(fp, "\n[Ticket]\n");

    /* [Account] 段：默认管理员账户，用户名 admin 密码 admin，角色 0=管理员 */
    fprintf(fp, "\n[Account]\n");
    fprintf(fp, "1|admin|admin|0\n");

    /* [Key] 段：下一个可用 ID，Studio=4 表示新演出厅从 4 开始，Account=2 表示新账户从 2 开始 */
    fprintf(fp, "\n[Key]\n");
    fprintf(fp, "Studio=4\n");
    fprintf(fp, "Seat=1\n");
    fprintf(fp, "Play=1\n");
    fprintf(fp, "Schedule=1\n");
    fprintf(fp, "Ticket=1\n");
    fprintf(fp, "Account=2\n");

    fclose(fp);
    printf("data.txt: 3 studios, admin account (admin/admin)\n");
    printf("Done.\n");
    return 0;
}
