/*
 * web_server.c — TTMS Web 后端（纯 C + Winsock）
 * 作用：本机 HTTP 服务，供 index.html 调用 REST 风格 JSON API。
 * 端口：HTTP_PORT（默认 8765）；数据文件 data.txt 与控制台版共用。
 * 认证：内置管理员 admin/admin（不写 data.txt）；业务接口需请求头 X-User-Id、X-User-Role。
 * 静态文件：URL 以 /static/ 开头时读项目下 static 目录；否则先尝试 static/ 再尝试根目录。
 */

#define WIN32_LEAN_AND_MEAN /* 精简 Windows 头，加快编译、减少宏污染 */
#include <winsock2.h>       /* 套接字 API，须在 windows.h 之前 */
#include <ws2tcpip.h>       /* getaddrinfo 等扩展（本文件主要用 sockaddr_in） */
#include <windows.h>        /* MultiByteToWideChar、GetModuleFileName、控制台代码页等 */
#include <stdio.h>          /* printf、fprintf、文件读写、snprintf */
#include <stdlib.h>         /* atoi、malloc、strtod */
#include <string.h>         /* strcmp、strstr、memcpy、memset */
#include <ctype.h>          /* 若需字符判断（本文件少量逻辑可用） */

#ifndef strcasecmp
#define strcasecmp _stricmp /* MSVC/MinGW 下忽略大小写的 strcmp */
#endif

#include "src/Common/List.h"
#include "src/Service/Studio.h"
#include "src/Service/Seat.h"
#include "src/Service/Play.h"
#include "src/Service/Schedule.h"
#include "src/Service/Ticket.h"
#include "src/Service/Account.h"
#include "src/Persistence/Data_Store.h"

#pragma comment(lib, "ws2_32.lib") /* 链接 Winsock 导入库 */

#define HTTP_PORT 8765                 /* 监听端口，仅绑定 127.0.0.1 */
#define BUILTIN_ADMIN_ID 0             /* 内置管理员在 JSON 里使用的 id */
#define BUILTIN_ADMIN_USER "admin"     /* 内置管理员用户名 */
#define BUILTIN_ADMIN_PASS "admin"     /* 内置管理员密码（仅演示，勿上公网） */
#define REQ_BUFSZ (256 * 1024)         /* 单次请求头+正文的接收缓冲上限 */
#define RESP_BUFSZ (600 * 1024)        /* 拼装 JSON 响应的缓冲上限 */
#define FILE_IO_CHUNK (64 * 1024)      /* 静态文件分块读取、发送的块大小 */

static char g_resp[RESP_BUFSZ]; /* 全局：生成 JSON 后由此 send_json 发出 */
static char g_body[REQ_BUFSZ];  /* 全局：POST 正文解析用 */

/* 校验是否为内置管理员账号（明文比对） */
static int verify_builtin_admin(const char *username, const char *password) {
	return username && password /* 非空指针 */
		&& strcmp(username, BUILTIN_ADMIN_USER) == 0 /* 用户名一致 */
		&& strcmp(password, BUILTIN_ADMIN_PASS) == 0; /* 密码一致 */
}

/* HTTP 状态码对应的原因短语（仅响应用） */
static const char *http_reason(int code) {
	switch (code) {
		case 200: return "OK";
		case 204: return "No Content";
		case 400: return "Bad Request";
		case 401: return "Unauthorized";
		case 403: return "Forbidden";
		case 404: return "Not Found";
		default: return "OK";
	}
}

/* 将缓冲区 p 中 len 字节全部写入套接字（处理 send 只发一部分的情况） */
static int send_all(SOCKET s, const char *p, int len) {
	int sent = 0; /* 已发送字节数 */
	while (sent < len) { /* 直到发完 */
		int n = send(s, p + sent, len - sent, 0); /* 从偏移 sent 起发送 */
		if (n <= 0) return 0; /* 失败或连接关闭 */
		sent += n; /* 累加 */
	}
	return 1; /* 成功 */
}

/* 发送完整 HTTP 响应：状态行 + 头 + 可选正文（正文可为二进制，长度由 blen 指定） */
static void send_raw(SOCKET s, int code, const char *ctype, const char *body, int blen) {
	char hdr[512]; /* 响应头缓冲 */
	int hlen = snprintf(hdr, sizeof hdr,
		"HTTP/1.1 %d %s\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %d\r\n"
		"Access-Control-Allow-Origin: *\r\n"
		"Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n"
		"Access-Control-Allow-Headers: Content-Type, X-User-Id, X-User-Role\r\n"
		"Connection: close\r\n\r\n",
		code, http_reason(code), ctype, blen);
	if (hlen > 0 && hlen < (int)sizeof hdr) send_all(s, hdr, hlen); /* 先发头 */
	if (blen > 0 && body) send_all(s, body, blen); /* 再发正文 */
}

/*
 * 按磁盘原样流式发送文件：不整段读入大缓冲、不写入 '\\0'，
 * 避免大图被截断或二进制被误当 C 字符串处理。
 */
static int send_file_stream(SOCKET s, int code, const char *ctype, FILE *fp) {
	long flen; /* 文件长度 */
	char buf[FILE_IO_CHUNK]; /* 读文件块 */

	if (fseek(fp, 0, SEEK_END) != 0) return 0; /* 定位到末尾失败 */
	flen = ftell(fp); /* 当前位置即长度 */
	if (flen < 0) return 0; /* ftell 失败 */
	if (fseek(fp, 0, SEEK_SET) != 0) return 0; /* 回到文件头 */

	char hdr[512];
	int hlen = snprintf(hdr, sizeof hdr,
		"HTTP/1.1 %d %s\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %ld\r\n"
		"X-Content-Type-Options: nosniff\r\n"
		"Access-Control-Allow-Origin: *\r\n"
		"Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n"
		"Access-Control-Allow-Headers: Content-Type, X-User-Id, X-User-Role\r\n"
		"Connection: close\r\n\r\n",
		code, http_reason(code), ctype, flen);
	if (hlen <= 0 || hlen >= (int)sizeof hdr) return 0; /* 头过长或格式化失败 */
	if (!send_all(s, hdr, hlen)) return 0; /* 发送响应头 */

	long rem = flen; /* 剩余要发送的字节数 */
	while (rem > 0) { /* 分块读并发 */
		size_t want = (size_t)(rem > (long)sizeof buf ? sizeof buf : (size_t)rem); /* 本块大小 */
		size_t rd = fread(buf, 1, want, fp); /* 从文件读 */
		if (rd == 0) return 0; /* 未读到预期数据则失败 */
		if (!send_all(s, buf, (int)rd)) return 0; /* 发送本块 */
		rem -= (long)rd; /* 减少剩余 */
	}
	return 1;
}

/* 发送 UTF-8 JSON 文本（长度用 strlen，正文须为合法 C 字符串） */
static void send_json(SOCKET s, int code, const char *json) {
	int n = json ? (int)strlen(json) : 0; /* 空指针当空对象 */
	send_raw(s, code, "application/json; charset=utf-8", json ? json : "{}", n);
}

/* 响应浏览器的 CORS 预检 OPTIONS 请求 */
static void send_options(SOCKET s) {
	static const char *h =
		"HTTP/1.1 204 No Content\r\n"
		"Access-Control-Allow-Origin: *\r\n"
		"Access-Control-Allow-Methods: GET, POST, DELETE, OPTIONS\r\n"
		"Access-Control-Allow-Headers: Content-Type, X-User-Id, X-User-Role\r\n"
		"Content-Length: 0\r\nConnection: close\r\n\r\n";
	send_all(s, h, (int)strlen(h));
}

/* 仅对 JSON 字符串做最小转义；in 须已是 UTF-8 字节序列 */
static void json_esc_raw(char *out, size_t cap, const char *in) {
	size_t j = 0; /* 写入 out 的下标 */
	if (!in) in = ""; /* 空指针当空串 */
	while (*in && j + 2 < cap) { /* 留空间给可能的反斜杠与结尾 0 */
		unsigned char c = (unsigned char)*in++; /* 当前字节 */
		if (c == '"' || c == '\\') { /* JSON 需转义的字符 */
			out[j++] = '\\';
			if (j >= cap - 1) break;
		}
		out[j++] = (char)(c < 32 ? ' ' : c); /* 控制字符改成空格 */
	}
	out[j] = '\0'; /* 结束 C 字符串 */
}

/*
 * data.txt 在记事本等环境下常保存为系统 ANSI（简体中文 Windows 多为 GBK），
 * 而 HTTP JSON 声明 charset=utf-8。若把 ANSI 字节当 UTF-8 发给浏览器会乱码。
 * 策略：能通过 UTF-8 严格校验则原样拷贝；否则按 CP_ACP 解码再转为 UTF-8。
 */
static void string_to_utf8(const char *in, char *out, size_t cap) {
	if (!out || cap == 0) return; /* 无效输出 */
	out[0] = '\0';
	if (!in || !*in) return; /* 空输入 */

	if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, in, -1, NULL, 0) > 0) { /* 已是合法 UTF-8 */
		size_t k = 0;
		while (in[k] && k + 1 < cap) {
			out[k] = in[k];
			k++;
		}
		out[k] = '\0';
		return;
	}

	int n_w = MultiByteToWideChar(CP_ACP, 0, in, -1, NULL, 0); /* ANSI 转宽字符所需 wchar 数 */
	if (n_w <= 0 || n_w > 512) { /* 失败或过长则退回逐字节拷贝 */
		size_t k = 0;
		while (in[k] && k + 1 < cap) {
			out[k] = in[k];
			k++;
		}
		out[k] = '\0';
		return;
	}
	wchar_t wbuf[512];
	if (MultiByteToWideChar(CP_ACP, 0, in, -1, wbuf, n_w) <= 0) { /* 填充宽字符缓冲 */
		size_t k = 0;
		while (in[k] && k + 1 < cap) {
			out[k] = in[k];
			k++;
		}
		out[k] = '\0';
		return;
	}
	if (WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, out, (int)cap, NULL, NULL) <= 0) { /* 宽字符转 UTF-8 */
		size_t k = 0;
		while (in[k] && k + 1 < cap) {
			out[k] = in[k];
			k++;
		}
		out[k] = '\0';
	}
}

/* 先把字段转成 UTF-8，再写入 JSON 字符串转义缓冲 */
static void json_esc_utf8(char *out, size_t cap, const char *in) {
	char tmp[512];
	string_to_utf8(in, tmp, sizeof tmp);
	json_esc_raw(out, cap, tmp);
}

/* 跳过 JSON 中的空白字符 */
static const char *json_skip(const char *p) {
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
	return p;
}

/* 在简单 JSON 正文中查找 "\"key\"" 后的值起始位置（冒号后） */
static const char *json_find_value(const char *body, const char *key) {
	char pat[96];
	snprintf(pat, sizeof pat, "\"%s\"", key);
	const char *p = strstr(body, pat);
	if (!p) return NULL;
	p += strlen(pat);
	p = json_skip(p);
	if (*p != ':') return NULL;
	p++;
	return json_skip(p);
}

/* 解析 JSON 字符串值（支持简单 \" 转义）到 out */
static int json_get_string(const char *body, const char *key, char *out, size_t olen) {
	const char *p = json_find_value(body, key);
	if (!p || *p != '"') return 0;
	p++;
	size_t i = 0;
	while (*p && *p != '"' && i + 1 < olen) {
		if (*p == '\\' && p[1]) p++;
		out[i++] = *p++;
	}
	out[i] = '\0';
	return 1;
}

/* 解析 JSON 数字为 int（从值起始处 atoi） */
static int json_get_int(const char *body, const char *key, int *v) {
	const char *p = json_find_value(body, key);
	if (!p) return 0;
	*v = atoi(p);
	return 1;
}

/* 解析 JSON 数字为 float */
static int json_get_float(const char *body, const char *key, float *v) {
	const char *p = json_find_value(body, key);
	if (!p) return 0;
	*v = (float)strtod(p, NULL);
	return 1;
}

/* 从 HTTP 头中读取整型字段，例如 Content-Length */
static int header_int(const char *req, const char *name, int *out) {
	char pat[64];
	snprintf(pat, sizeof pat, "\r\n%s:", name);
	const char *p = strstr(req, pat);
	if (!p) return 0;
	p += strlen(pat);
	while (*p == ' ' || *p == '\t') p++;
	*out = atoi(p);
	return 1;
}

/* 从 HTTP 头中读取一行字符串值（到 CRLF 为止） */
static int header_str(const char *req, const char *name, char *out, size_t cap) {
	char pat[64];
	snprintf(pat, sizeof pat, "\r\n%s:", name);
	const char *p = strstr(req, pat);
	if (!p) return 0;
	p += strlen(pat);
	while (*p == ' ' || *p == '\t') p++;
	size_t i = 0;
	while (*p && *p != '\r' && *p != '\n' && i + 1 < cap)
		out[i++] = *p++;
	out[i] = '\0';
	return 1;
}

/* 解析查询字符串中的 key=value（value 遇 & 结束） */
static int query_param(const char *q, const char *key, char *out, size_t cap) {
	if (!q || !out || cap == 0) return 0;
	char pat[48];
	snprintf(pat, sizeof pat, "%s=", key);
	const char *p = strstr(q, pat);
	if (!p) return 0;
	p += strlen(pat);
	size_t i = 0;
	while (*p && *p != '&' && i + 1 < cap)
		out[i++] = *p++;
	out[i] = '\0';
	return 1;
}

/* 查询参数转 int */
static int query_param_int(const char *q, const char *key, int *v) {
	char buf[32];
	if (!query_param(q, key, buf, sizeof buf)) return 0;
	*v = atoi(buf);
	return 1;
}

/* 从请求头读取当前登录用户 id 与角色（前端登录后设置） */
static int user_from_request(const char *req, int *uid, int *role) {
	char ids[32], rs[32];
	if (!header_str(req, "X-User-Id", ids, sizeof ids)) return 0;
	if (!header_str(req, "X-User-Role", rs, sizeof rs)) *role = 1;
	else *role = atoi(rs);
	*uid = atoi(ids);
	return *uid > 0 || (*uid == 0 && strcmp(ids, "0") == 0);
}

/* 生成 GET /api/studios 的 JSON 数组：含每厅剩余可售座位估算 */
static void json_studios_array(void) {
	studio_list_t L;
	List_Init(L, studio_node_t);
	Studio_Srv_FetchAll(L);
	char *w = g_resp, *end = g_resp + RESP_BUFSZ - 8;
	*w++ = '[';
	int first = 1;
	studio_node_t *pn;
	List_ForEach(L, pn) {
		seat_list_t SL;
		List_Init(SL, seat_node_t);
		Seat_Srv_FetchByRoomID(SL, pn->data.id);
		int damaged = 0, occupied = 0;
		seat_node_t *sp;
		List_ForEach(SL, sp) {
			if (sp->data.status == SEAT_BROKEN) damaged++;
			else if (sp->data.status == SEAT_GOOD) occupied++;
		}
		List_Destroy(SL, seat_node_t);
		int rem = pn->data.seatsCount - damaged - occupied;
		if (rem < 0) rem = 0;
		char en[80];
		json_esc_utf8(en, sizeof en, pn->data.name);
		if (!first) *w++ = ',';
		first = 0;
		w += snprintf(w, (size_t)(end - w),
			"{\"id\":%d,\"name\":\"%s\",\"rows\":%d,\"cols\":%d,\"seatsCount\":%d,\"remainingSeats\":%d}",
			pn->data.id, en, pn->data.rowsCount, pn->data.colsCount, pn->data.seatsCount, rem);
		if (w >= end) break;
	}
	*w++ = ']';
	*w = '\0';
	List_Destroy(L, studio_node_t);
}

/* 生成 GET /api/plays 的剧目列表 JSON */
static void json_plays_array(void) {
	play_list_t L;
	List_Init(L, play_node_t);
	Play_Srv_FetchAll(L);
	char *w = g_resp, *end = g_resp + RESP_BUFSZ - 8;
	*w++ = '[';
	int first = 1;
	play_node_t *p;
	List_ForEach(L, p) {
		char na[120], ty[48];
		json_esc_utf8(na, sizeof na, p->data.name);
		json_esc_utf8(ty, sizeof ty, p->data.type);
		if (!first) *w++ = ',';
		first = 0;
		w += snprintf(w, (size_t)(end - w), "{\"id\":%d,\"name\":\"%s\",\"type\":\"%s\",\"duration\":%d}",
			p->data.id, na, ty, p->data.duration);
		if (w >= end) break;
	}
	*w++ = ']';
	*w = '\0';
	List_Destroy(L, play_node_t);
}

/* 生成 GET /api/schedules 的排期列表 JSON */
static void json_schedules_array(void) {
	schedule_list_t L;
	List_Init(L, schedule_node_t);
	Schedule_Srv_FetchAll(L);
	char *w = g_resp, *end = g_resp + RESP_BUFSZ - 8;
	*w++ = '[';
	int first = 1;
	schedule_node_t *p;
	List_ForEach(L, p) {
		char d[32], t[16];
		json_esc_utf8(d, sizeof d, p->data.date);
		json_esc_utf8(t, sizeof t, p->data.time);
		if (!first) *w++ = ',';
		first = 0;
		w += snprintf(w, (size_t)(end - w),
			"{\"id\":%d,\"playID\":%d,\"studioID\":%d,\"date\":\"%s\",\"time\":\"%s\",\"price\":%.2f}",
			p->data.id, p->data.playID, p->data.studioID, d, t, (double)p->data.price);
		if (w >= end) break;
	}
	*w++ = ']';
	*w = '\0';
	List_Destroy(L, schedule_node_t);
}

/* 生成某影厅座位列表：GET /api/seats?room= */
static void json_seats_for_room(int roomId) {
	seat_list_t L;
	List_Init(L, seat_node_t);
	Seat_Srv_FetchByRoomID(L, roomId);
	char *w = g_resp, *end = g_resp + RESP_BUFSZ - 8;
	*w++ = '[';
	int first = 1;
	seat_node_t *p;
	List_ForEach(L, p) {
		if (!first) *w++ = ',';
		first = 0;
		w += snprintf(w, (size_t)(end - w),
			"{\"id\":%d,\"roomID\":%d,\"row\":%d,\"col\":%d,\"status\":%d}",
			p->data.id, p->data.roomID, p->data.row, p->data.column, (int)p->data.status);
		if (w >= end) break;
	}
	*w++ = ']';
	*w = '\0';
	List_Destroy(L, seat_node_t);
}

/* 合并所有影厅座位为一个大数组：GET /api/seats 无 room 参数时 */
static void json_seats_all(void) {
	seat_list_t L;
	List_Init(L, seat_node_t);
	studio_list_t SL;
	List_Init(SL, studio_node_t);
	Studio_Srv_FetchAll(SL);
	studio_node_t *sn;
	List_ForEach(SL, sn) {
		seat_list_t TL;
		List_Init(TL, seat_node_t);
		Seat_Srv_FetchByRoomID(TL, sn->data.id);
		seat_node_t *tp;
		List_ForEach(TL, tp) {
			seat_node_t *nn = (seat_node_t *)malloc(sizeof *nn);
			if (nn) {
				nn->data = tp->data;
				List_AddTail(L, nn);
			}
		}
		List_Destroy(TL, seat_node_t);
	}
	List_Destroy(SL, studio_node_t);
	char *w = g_resp, *end = g_resp + RESP_BUFSZ - 8;
	*w++ = '[';
	int first = 1;
	seat_node_t *p;
	List_ForEach(L, p) {
		if (!first) *w++ = ',';
		first = 0;
		w += snprintf(w, (size_t)(end - w),
			"{\"id\":%d,\"roomID\":%d,\"row\":%d,\"col\":%d,\"status\":%d}",
			p->data.id, p->data.roomID, p->data.row, p->data.column, (int)p->data.status);
		if (w >= end) break;
	}
	*w++ = ']';
	*w = '\0';
	List_Destroy(L, seat_node_t);
}

/* 某场次票务列表；refundOnly 时普通用户只看自己的票 */
static void json_tickets_filtered(int scheduleId, int refundOnly, int userId, int userRole) {
	ticket_list_t L;
	List_Init(L, ticket_node_t);
	Ticket_Srv_FetchByScheduleID(L, scheduleId);
	char *w = g_resp, *end = g_resp + RESP_BUFSZ - 8;
	*w++ = '[';
	int first = 1;
	ticket_node_t *p;
	List_ForEach(L, p) {
		if (refundOnly && userRole != 0 && p->data.accountID != userId) continue;
		if (!first) *w++ = ',';
		first = 0;
		w += snprintf(w, (size_t)(end - w),
			"{\"id\":%d,\"scheduleID\":%d,\"seatID\":%d,\"status\":%d,\"accountID\":%d}",
			p->data.id, p->data.scheduleID, p->data.seatID, (int)p->data.status, p->data.accountID);
		if (w >= end) break;
	}
	*w++ = ']';
	*w = '\0';
	List_Destroy(L, ticket_node_t);
}

/* 全部票务或仅当前用户（非管理员） */
static void json_tickets_all_or_mine(int userId, int userRole) {
	ticket_list_t L;
	List_Init(L, ticket_node_t);
	Ticket_Srv_FetchAll(L);
	char *w = g_resp, *end = g_resp + RESP_BUFSZ - 8;
	*w++ = '[';
	int first = 1;
	ticket_node_t *p;
	List_ForEach(L, p) {
		if (userRole != 0 && p->data.accountID != userId) continue;
		if (!first) *w++ = ',';
		first = 0;
		w += snprintf(w, (size_t)(end - w),
			"{\"id\":%d,\"scheduleID\":%d,\"seatID\":%d,\"status\":%d,\"accountID\":%d}",
			p->data.id, p->data.scheduleID, p->data.seatID, (int)p->data.status, p->data.accountID);
		if (w >= end) break;
	}
	*w++ = ']';
	*w = '\0';
	List_Destroy(L, ticket_node_t);
}

/* 账户列表：管理员看全部 data 用户；普通用户只看自己；跳过内置 admin */
static void json_accounts_filtered(int userId, int userRole) {
	account_list_t L;
	List_Init(L, account_node_t);
	Account_Srv_FetchAll(L);
	char *w = g_resp, *end = g_resp + RESP_BUFSZ - 8;
	*w++ = '[';
	int first = 1;
	account_node_t *p;
	List_ForEach(L, p) {
		if (userRole != 0 && p->data.id != userId) continue;
		if (strcmp(p->data.username, BUILTIN_ADMIN_USER) == 0) continue;
		if (!first) *w++ = ',';
		first = 0;
		char eu[64];
		json_esc_utf8(eu, sizeof eu, p->data.username);
		char ep[64];
		json_esc_utf8(ep, sizeof ep, p->data.password);
		w += snprintf(w, (size_t)(end - w),
			"{\"id\":%d,\"username\":\"%s\",\"password\":\"%s\",\"role\":%d}",
			p->data.id, eu, ep, (int)p->data.role);
		if (w >= end) break;
	}
	*w++ = ']';
	*w = '\0';
	List_Destroy(L, account_node_t);
}

/* 按剧目汇总售票金额排行（管理员看全局；用户只看与自己相关的票） */
static void json_ranking(int userId, int userRole) {
	play_list_t PL;
	List_Init(PL, play_node_t);
	Play_Srv_FetchAll(PL);
	schedule_list_t SL;
	List_Init(SL, schedule_node_t);
	Schedule_Srv_FetchAll(SL);
	ticket_list_t TL;
	List_Init(TL, ticket_node_t);
	Ticket_Srv_FetchAll(TL);
	typedef struct { int id; char name[128]; float amt; } row_t;
	row_t rows[256];
	int nrows = 0;
	play_node_t *pp;
	List_ForEach(PL, pp) {
		if (nrows >= 256) break;
		float total = 0;
		schedule_node_t *sc;
		List_ForEach(SL, sc) {
			if (sc->data.playID != pp->data.id) continue;
			int sold = 0;
			ticket_node_t *tk;
			List_ForEach(TL, tk) {
				if (tk->data.scheduleID != sc->data.id || tk->data.status != TICKET_SOLD) continue;
				if (userRole != 0 && tk->data.accountID != userId) continue;
				sold++;
			}
			total += (float)sold * sc->data.price;
		}
		if (userRole == 0 || total > 0.001f) {
			rows[nrows].id = pp->data.id;
			strncpy(rows[nrows].name, pp->data.name, sizeof rows[nrows].name - 1);
			rows[nrows].name[sizeof rows[nrows].name - 1] = '\0';
			rows[nrows].amt = total;
			nrows++;
		}
	}
	for (int i = 0; i < nrows; i++)
		for (int j = i + 1; j < nrows; j++)
			if (rows[j].amt > rows[i].amt) {
				row_t t = rows[i];
				rows[i] = rows[j];
				rows[j] = t;
			}
	char *w = g_resp, *end = g_resp + RESP_BUFSZ - 8;
	*w++ = '[';
	for (int i = 0; i < nrows; i++) {
		char en[160];
		json_esc_utf8(en, sizeof en, rows[i].name);
		if (i) *w++ = ',';
		w += snprintf(w, (size_t)(end - w), "{\"id\":%d,\"name\":\"%s\",\"totalAmount\":%.2f}",
			rows[i].id, en, (double)rows[i].amt);
		if (w >= end) break;
	}
	*w++ = ']';
	*w = '\0';
	List_Destroy(PL, play_node_t);
	List_Destroy(SL, schedule_node_t);
	List_Destroy(TL, ticket_node_t);
}

/* 查找某场次某座位对应的未售票记录，用于售票逻辑 */
static int find_unsold_ticket(int scheduleId, int seatId, ticket_t *out) {
	ticket_list_t L;
	List_Init(L, ticket_node_t);
	Ticket_Srv_FetchByScheduleID(L, scheduleId);
	ticket_node_t *p;
	int ok = 0;
	List_ForEach(L, p) {
		if (p->data.seatID == seatId && p->data.status == TICKET_UNSOLD) {
			*out = p->data;
			ok = 1;
			break;
		}
	}
	List_Destroy(L, ticket_node_t);
	return ok;
}

/* DELETE /api/{entity}/{id}：按类型级联删除 */
static int handle_delete_entity(const char *entity, int eid, int userId, int userRole) {
	if (strcmp(entity, "studio") == 0) {
		Seat_Srv_DeleteAllByRoomID(eid);
		return Studio_Srv_DeleteByID(eid);
	}
	if (strcmp(entity, "play") == 0) {
		schedule_list_t L;
		List_Init(L, schedule_node_t);
		Schedule_Srv_FetchAll(L);
		int ids[512], n = 0;
		schedule_node_t *p;
		List_ForEach(L, p) {
			if (p->data.playID == eid && n < 512) ids[n++] = p->data.id;
		}
		List_Destroy(L, schedule_node_t);
		for (int i = 0; i < n; i++)
			Schedule_Srv_DeleteByID(ids[i]);
		return Play_Srv_DeleteByID(eid);
	}
	if (strcmp(entity, "schedule") == 0) {
		if (userRole != 0) return 0;
		return Schedule_Srv_DeleteByID(eid);
	}
	if (strcmp(entity, "account") == 0) {
		if (userRole != 0) return 0;
		if (eid == userId) return 0;
		if (eid == BUILTIN_ADMIN_ID) return 0;
		return Account_Srv_DeleteByID(eid);
	}
	return 0;
}

/*
 * 提供静态文件：GET 非 /api/ 的请求。
 * 根路径返回 index.html；/static/ 映射到目录 static；其它先试 static/ 再试根目录。
 */
static int serve_local_file(SOCKET s, const char *url_path) {
	char path[512];
	FILE *f;
	if (strcmp(url_path, "/") == 0 || strcmp(url_path, "") == 0) {
		strncpy(path, "index.html", sizeof path - 1);
		path[sizeof path - 1] = '\0';
		f = fopen(path, "rb");
	} else {
		if (strstr(url_path, "..") || url_path[0] != '/')
			return 0;
		const char *rel = url_path + 1;
		if (strncmp(rel, "static/", 7) == 0) {
			snprintf(path, sizeof path, "%s", rel);
			f = fopen(path, "rb");
		} else {
			snprintf(path, sizeof path, "static/%s", rel);
			f = fopen(path, "rb");
			if (!f) {
				snprintf(path, sizeof path, "%s", rel);
				f = fopen(path, "rb");
			}
		}
	}
	if (!f) return 0;
	const char *ct = "application/octet-stream";
	const char *dot = strrchr(path, '.');
	if (dot) {
		if (strcasecmp(dot, ".html") == 0) ct = "text/html; charset=utf-8";
		else if (strcasecmp(dot, ".css") == 0) ct = "text/css; charset=utf-8";
		else if (strcasecmp(dot, ".js") == 0) ct = "application/javascript; charset=utf-8";
		else if (strcasecmp(dot, ".png") == 0) ct = "image/png";
		else if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) ct = "image/jpeg";
		else if (strcasecmp(dot, ".gif") == 0) ct = "image/gif";
	}
	if (!send_file_stream(s, 200, ct, f)) {
		fclose(f);
		return 0;
	}
	fclose(f);
	return 1;
}

/*
 * 处理单个 TCP 连接上的一次 HTTP 请求（读完头与正文后路由到 API 或静态文件）。
 * 约定：短连接，响应后由外层 closesocket。
 */
static void handle_request(SOCKET client) {
	char req[REQ_BUFSZ];
	int total = 0;
	for (;;) {
		int n = recv(client, req + total, (int)(sizeof req - 1 - (size_t)total), 0);
		if (n <= 0) break;
		total += n;
		if (total >= (int)sizeof req - 1) break;
		req[total] = '\0';
		if (strstr(req, "\r\n\r\n")) break;
	}
	req[total] = '\0';

	char method[16] = "GET";
	char path[512] = "/";
	if (sscanf(req, "%15s %511s", method, path) < 2) {
		send_json(client, 400, "{\"error\":\"bad request\"}");
		return;
	}

	char *qm = strchr(path, '?');
	char *query = NULL;
	if (qm) {
		*qm = '\0';
		query = qm + 1;
	}

	const char *body0 = strstr(req, "\r\n\r\n");
	const char *body = body0 ? body0 + 4 : "";
	int content_len = 0;
	{
		const char *cl = strstr(req, "Content-Length:");
		if (cl) {
			cl += 15;
			while (*cl == ' ' || *cl == '\t') cl++;
			content_len = atoi(cl);
		}
	}
	if (content_len > 0 && content_len < (int)sizeof g_body) {
		const char *hdr_end = strstr(req, "\r\n\r\n");
		int have = hdr_end ? (int)(total - (hdr_end + 4 - req)) : 0;
		if (have < 0) have = 0;
		int tocopy = have > content_len ? content_len : have;
		memcpy(g_body, body, (size_t)tocopy);
		int got = tocopy;
		while (got < content_len && got < (int)sizeof g_body - 1) {
			int n = recv(client, g_body + got, (int)sizeof g_body - 1 - got, 0);
			if (n <= 0) break;
			got += n;
		}
		g_body[got] = '\0';
		body = g_body;
	} else if (body && body != g_body) {
		size_t bl = strlen(body);
		if (bl >= sizeof g_body) bl = sizeof g_body - 1;
		memcpy(g_body, body, bl);
		g_body[bl] = '\0';
		body = g_body;
	}

	if (strcmp(method, "OPTIONS") == 0) {
		send_options(client);
		return;
	}

	/* ---------- 静态文件（图片、index.html 等），非 /api/ 的 GET ---------- */
	if (strcmp(method, "GET") == 0 && strncmp(path, "/api/", 5) != 0) {
		if (serve_local_file(client, path)) return;
	}

	/* ---------- 登录、注册：不需要 X-User-Id ---------- */
	if (strcmp(method, "POST") == 0 && strcmp(path, "/api/login") == 0) {
		char user[64], pass[128];
		json_get_string(body, "username", user, sizeof user);
		json_get_string(body, "password", pass, sizeof pass);
		if (!user[0] || !pass[0]) {
			send_json(client, 200, "{\"ok\":false,\"msg\":\"请输入用户名和密码\"}");
			return;
		}
		if (verify_builtin_admin(user, pass)) {
			snprintf(g_resp, sizeof g_resp,
				"{\"ok\":true,\"user\":{\"id\":%d,\"username\":\"%s\",\"role\":0}}",
				BUILTIN_ADMIN_ID, BUILTIN_ADMIN_USER);
			send_json(client, 200, g_resp);
			return;
		}
		account_t acc;
		if (Account_Srv_FetchByUsername(user, &acc) && strcmp(acc.password, pass) == 0) {
			char eu[80];
			json_esc_utf8(eu, sizeof eu, acc.username);
			snprintf(g_resp, sizeof g_resp,
				"{\"ok\":true,\"user\":{\"id\":%d,\"username\":\"%s\",\"role\":%d}}",
				acc.id, eu, (int)acc.role);
			send_json(client, 200, g_resp);
			return;
		}
		send_json(client, 200, "{\"ok\":false,\"msg\":\"用户名或密码错误\"}");
		return;
	}

	if (strcmp(method, "POST") == 0 && strcmp(path, "/api/register") == 0) {
		char user[64], pass[128];
		json_get_string(body, "username", user, sizeof user);
		json_get_string(body, "password", pass, sizeof pass);
		if (!user[0]) {
			send_json(client, 200, "{\"ok\":false,\"msg\":\"用户名不能为空\"}");
			return;
		}
		if ((int)strlen(pass) < 6) {
			send_json(client, 200, "{\"ok\":false,\"msg\":\"密码至少 6 位\"}");
			return;
		}
		if (strcmp(user, BUILTIN_ADMIN_USER) == 0) {
			send_json(client, 200, "{\"ok\":false,\"msg\":\"该用户名为系统保留\"}");
			return;
		}
		account_t tmp;
		if (Account_Srv_FetchByUsername(user, &tmp)) {
			send_json(client, 200, "{\"ok\":false,\"msg\":\"用户名已存在\"}");
			return;
		}
		account_t a;
		memset(&a, 0, sizeof a);
		strncpy(a.username, user, sizeof a.username - 1);
		strncpy(a.password, pass, sizeof a.password - 1);
		a.role = ACCOUNT_USER;
		if (!Account_Srv_Add(&a)) {
			send_json(client, 200, "{\"ok\":false,\"msg\":\"创建失败\"}");
			return;
		}
		char eu[80];
		json_esc_utf8(eu, sizeof eu, a.username);
		snprintf(g_resp, sizeof g_resp,
			"{\"ok\":true,\"msg\":\"创建成功\",\"user\":{\"id\":%d,\"username\":\"%s\",\"role\":1}}",
			a.id, eu);
		send_json(client, 200, g_resp);
		return;
	}

	/* ---------- 以下接口需已登录（请求头带用户 id/角色） ---------- */
	int uid = 0, role = 1;
	if (!user_from_request(req, &uid, &role)) {
		send_json(client, 401, "{\"ok\":false,\"msg\":\"请先登录\",\"code\":401}");
		return;
	}

	/* ---------- 只读资源 GET ---------- */
	if (strcmp(method, "GET") == 0 && strcmp(path, "/api/studios") == 0) {
		json_studios_array();
		send_json(client, 200, g_resp);
		return;
	}
	if (strcmp(method, "GET") == 0 && strcmp(path, "/api/plays") == 0) {
		json_plays_array();
		send_json(client, 200, g_resp);
		return;
	}
	if (strcmp(method, "GET") == 0 && strcmp(path, "/api/schedules") == 0) {
		json_schedules_array();
		send_json(client, 200, g_resp);
		return;
	}
	if (strcmp(method, "GET") == 0 && strcmp(path, "/api/seats") == 0) {
		char roombuf[32];
		if (query_param(query, "room", roombuf, sizeof roombuf)) {
			json_seats_for_room(atoi(roombuf));
			send_json(client, 200, g_resp);
			return;
		}
		json_seats_all();
		send_json(client, 200, g_resp);
		return;
	}
	if (strcmp(method, "GET") == 0 && strcmp(path, "/api/tickets") == 0) {
		char schbuf[32], refbuf[8];
		if (query_param(query, "schedule", schbuf, sizeof schbuf)) {
			int sid = atoi(schbuf);
			int refundOnly = query_param(query, "refund", refbuf, sizeof refbuf) && strcmp(refbuf, "1") == 0;
			json_tickets_filtered(sid, refundOnly, uid, role);
			send_json(client, 200, g_resp);
			return;
		}
		json_tickets_all_or_mine(uid, role);
		send_json(client, 200, g_resp);
		return;
	}
	if (strcmp(method, "GET") == 0 && strcmp(path, "/api/accounts") == 0) {
		json_accounts_filtered(uid, role);
		send_json(client, 200, g_resp);
		return;
	}
	if (strcmp(method, "GET") == 0 && strcmp(path, "/api/ranking") == 0) {
		json_ranking(uid, role);
		send_json(client, 200, g_resp);
		return;
	}

	/* ---------- DELETE /api/{studio|play|schedule|account}/{id} ---------- */
	if (strcmp(method, "DELETE") == 0 && strncmp(path, "/api/", 5) == 0) {
		char ent[32];
		int eid = 0;
		if (sscanf(path, "/api/%31[^/]/%d", ent, &eid) == 2) {
			if (handle_delete_entity(ent, eid, uid, role)) {
				send_json(client, 200, "{\"ok\":true,\"msg\":\"删除成功\"}");
			} else {
				send_json(client, 200, "{\"ok\":false,\"msg\":\"删除失败\"}");
			}
			return;
		}
		send_json(client, 404, "{\"error\":\"not found\"}");
		return;
	}

	/* ---------- 管理员清空 data ---------- */
	if (strcmp(method, "POST") == 0 && strcmp(path, "/api/init-all") == 0) {
		if (role != 0) {
			send_json(client, 200, "{\"ok\":false,\"msg\":\"仅管理员可执行此操作\"}");
			return;
		}
		Data_Store_ResetToEmpty();
		send_json(client, 200, "{\"ok\":true,\"msg\":\"初始化成功，已清空演出厅、剧目、排期、票务及 data 中全部账户；内置管理员仍可通过代码中的账号登录\"}");
		return;
	}

	/* ---------- 售票：占座并写票据 ---------- */
	if (strcmp(method, "POST") == 0 && strcmp(path, "/api/sell") == 0) {
		int scheduleID, row, col; /* 场次 id 与座位行列（从 0 起） */
		if (!json_get_int(body, "scheduleID", &scheduleID) || !json_get_int(body, "row", &row) || !json_get_int(body, "col", &col)) {
			send_json(client, 200, "{\"ok\":false,\"msg\":\"缺少参数\"}");
			return;
		}
		schedule_t sc; /* 排期记录 */
		if (!Schedule_Srv_FetchByID(scheduleID, &sc)) {
			send_json(client, 200, "{\"ok\":false,\"msg\":\"排期不存在\"}");
			return;
		}
		studio_t stu; /* 影厅尺寸，用于校验 row/col */
		if (!Studio_Srv_FetchByID(sc.studioID, &stu) || row < 0 || row >= stu.rowsCount || col < 0 || col >= stu.colsCount) {
			send_json(client, 200, "{\"ok\":false,\"msg\":\"座位范围无效\"}");
			return;
		}
		seat_list_t sl;
		List_Init(sl, seat_node_t);
		Seat_Srv_FetchByRoomID(sl, sc.studioID); /* 该厅全部座位节点 */
		seat_node_t *seatn = Seat_Srv_FindByRowCol(sl, row, col);
		int seat_id = 0; /* 最终用于出票的座位主键 */
		if (seatn) {
			if (seatn->data.status == SEAT_GOOD) { /* 已有人占用 */
				List_Destroy(sl, seat_node_t);
				send_json(client, 200, "{\"ok\":false,\"msg\":\"该座位已售\"}");
				return;
			}
			if (seatn->data.status == SEAT_BROKEN) {
				List_Destroy(sl, seat_node_t);
				send_json(client, 200, "{\"ok\":false,\"msg\":\"座位损坏\"}");
				return;
			}
			seat_id = seatn->data.id; /* 空位或占位，用已有座位 id */
		} else {
			seat_t ns; /* 库中尚无该行列记录则新建一条 */
			memset(&ns, 0, sizeof ns);
			ns.roomID = sc.studioID;
			ns.row = row;
			ns.column = col;
			ns.status = SEAT_GOOD;
			if (!Seat_Srv_Add(&ns)) {
				List_Destroy(sl, seat_node_t);
				send_json(client, 200, "{\"ok\":false,\"msg\":\"出票失败\"}");
				return;
			}
			seat_id = ns.id;
		}
		List_Destroy(sl, seat_node_t);

		ticket_t tk;
		if (find_unsold_ticket(scheduleID, seat_id, &tk)) { /* 已有未售票占位则改为已售 */
			tk.status = TICKET_SOLD;
			tk.accountID = uid;
			if (!Ticket_Srv_Modify(&tk)) {
				send_json(client, 200, "{\"ok\":false,\"msg\":\"出票失败\"}");
				return;
			}
		} else { /* 否则新建已售票 */
			memset(&tk, 0, sizeof tk);
			tk.scheduleID = scheduleID;
			tk.seatID = seat_id;
			tk.status = TICKET_SOLD;
			tk.accountID = uid;
			if (!Ticket_Srv_Add(&tk)) {
				send_json(client, 200, "{\"ok\":false,\"msg\":\"出票失败\"}");
				return;
			}
		}
		{
			seat_t srec; /* 售出后座位状态标为已售（SEAT_GOOD 表示有人） */
			if (Seat_Srv_FetchByID(seat_id, &srec)) {
				srec.status = SEAT_GOOD;
				Seat_Srv_Modify(&srec);
			}
		}
		snprintf(g_resp, sizeof g_resp, "{\"ok\":true,\"msg\":\"出票成功! 票价:%.2f 元\",\"price\":%.2f}", sc.price, sc.price);
		send_json(client, 200, g_resp);
		return;
	}

	/* ---------- 退票：释放座位并标记票已退 ---------- */
	if (strcmp(method, "POST") == 0 && strcmp(path, "/api/refund") == 0) {
		int scheduleID, row, col;
		if (!json_get_int(body, "scheduleID", &scheduleID) || !json_get_int(body, "row", &row) || !json_get_int(body, "col", &col)) {
			send_json(client, 200, "{\"ok\":false,\"msg\":\"缺少参数\"}");
			return;
		}
		schedule_t sc;
		if (!Schedule_Srv_FetchByID(scheduleID, &sc)) {
			send_json(client, 200, "{\"ok\":false,\"msg\":\"排期不存在\"}");
			return;
		}
		seat_list_t sl;
		List_Init(sl, seat_node_t);
		Seat_Srv_FetchByRoomID(sl, sc.studioID);
		seat_node_t *seatn = Seat_Srv_FindByRowCol(sl, row, col);
		if (!seatn) {
			List_Destroy(sl, seat_node_t);
			send_json(client, 200, "{\"ok\":false,\"msg\":\"座位不存在\"}");
			return;
		}
		ticket_list_t tl;
		List_Init(tl, ticket_node_t);
		Ticket_Srv_FetchByScheduleID(tl, scheduleID);
		ticket_node_t *tn = NULL;
		ticket_node_t *p;
		List_ForEach(tl, p) {
			if (p->data.seatID == seatn->data.id && p->data.status == TICKET_SOLD) {
				tn = p;
				break;
			}
		}
		if (!tn) {
			List_Destroy(tl, ticket_node_t);
			List_Destroy(sl, seat_node_t);
			send_json(client, 200, "{\"ok\":false,\"msg\":\"该场次此座位无已售可退票记录（以售票系统票据为准）\"}");
			return;
		}
		if (role != 0 && tn->data.accountID != uid) {
			List_Destroy(tl, ticket_node_t);
			List_Destroy(sl, seat_node_t);
			send_json(client, 200, "{\"ok\":false,\"msg\":\"只能退自己购买的票\"}");
			return;
		}
		{
			int tid = tn->data.id;
			int sid = seatn->data.id;
			ticket_t trec;
			seat_t srec;
			List_Destroy(tl, ticket_node_t);
			List_Destroy(sl, seat_node_t);
			if (Ticket_Srv_FetchByID(tid, &trec)) {
				trec.status = TICKET_REFUNDED;
				Ticket_Srv_Modify(&trec);
			}
			if (Seat_Srv_FetchByID(sid, &srec)) {
				srec.status = SEAT_NONE;
				Seat_Srv_Modify(&srec);
			}
		}
		send_json(client, 200, "{\"ok\":true,\"msg\":\"退票成功!\"}");
		return;
	}

	/* ---------- 影厅增改、初始化座位矩阵 ---------- */
	if (strcmp(method, "POST") == 0 && strcmp(path, "/api/studio") == 0) {
		char act[32];
		if (!json_get_string(body, "action", act, sizeof act)) {
			send_json(client, 200, "{\"ok\":false,\"msg\":\"未知操作\"}");
			return;
		}
		if (strcmp(act, "add") == 0) {
			char name[64];
			json_get_string(body, "name", name, sizeof name);
			int rows = 10, cols = 10;
			json_get_int(body, "rows", &rows);
			json_get_int(body, "cols", &cols);
			if (!name[0]) {
				send_json(client, 200, "{\"ok\":false,\"msg\":\"名称不能为空\"}");
				return;
			}
			studio_t s;
			memset(&s, 0, sizeof s);
			strncpy(s.name, name, sizeof s.name - 1);
			s.rowsCount = rows;
			s.colsCount = cols;
			s.seatsCount = 0;
			if (!Studio_Srv_Add(&s)) {
				send_json(client, 200, "{\"ok\":false,\"msg\":\"添加失败\"}");
				return;
			}
			snprintf(g_resp, sizeof g_resp, "{\"ok\":true,\"msg\":\"添加成功\",\"id\":%d}", s.id);
			send_json(client, 200, g_resp);
			return;
		}
		if (strcmp(act, "update") == 0) {
			int sid = 0;
			json_get_int(body, "id", &sid);
			studio_t s;
			if (!Studio_Srv_FetchByID(sid, &s)) {
				send_json(client, 200, "{\"ok\":false,\"msg\":\"演出厅不存在\"}");
				return;
			}
			char name[64];
			if (json_get_string(body, "name", name, sizeof name)) strncpy(s.name, name, sizeof s.name - 1);
			json_get_int(body, "rows", &s.rowsCount);
			json_get_int(body, "cols", &s.colsCount);
			s.id = sid;
			if (!Studio_Srv_Modify(&s)) {
				send_json(client, 200, "{\"ok\":false,\"msg\":\"修改失败\"}");
				return;
			}
			send_json(client, 200, "{\"ok\":true,\"msg\":\"修改成功\"}");
			return;
		}
		if (strcmp(act, "init-seats") == 0) {
			int sid = 0;
			json_get_int(body, "id", &sid);
			studio_t s;
			if (!Studio_Srv_FetchByID(sid, &s)) {
				send_json(client, 200, "{\"ok\":false,\"msg\":\"演出厅不存在\"}");
				return;
			}
			seat_list_t lst;
			List_Init(lst, seat_node_t);
			int n = Seat_Srv_RoomInit(lst, sid, s.rowsCount, s.colsCount);
			List_Destroy(lst, seat_node_t);
			snprintf(g_resp, sizeof g_resp, "{\"ok\":true,\"msg\":\"初始化 %d 个座位成功\"}", n);
			send_json(client, 200, g_resp);
			return;
		}
		send_json(client, 200, "{\"ok\":false,\"msg\":\"未知操作\"}");
		return;
	}

	/* ---------- 单座位状态（空/售/坏） ---------- */
	if (strcmp(method, "POST") == 0 && strcmp(path, "/api/seat") == 0) {
		char act[32];
		if (!json_get_string(body, "action", act, sizeof act) || strcmp(act, "set") != 0) {
			send_json(client, 200, "{\"ok\":false,\"msg\":\"未知操作\"}");
			return;
		}
		int roomID, row, col, status = 0;
		if (!json_get_int(body, "roomID", &roomID) || !json_get_int(body, "row", &row) || !json_get_int(body, "col", &col)) {
			send_json(client, 200, "{\"ok\":false,\"msg\":\"缺少参数\"}");
			return;
		}
		json_get_int(body, "status", &status);
		studio_t stu;
		if (!Studio_Srv_FetchByID(roomID, &stu) || row < 0 || row >= stu.rowsCount || col < 0 || col >= stu.colsCount) {
			send_json(client, 200, "{\"ok\":false,\"msg\":\"座位范围无效\"}");
			return;
		}
		seat_list_t sl;
		List_Init(sl, seat_node_t);
		Seat_Srv_FetchByRoomID(sl, roomID);
		seat_node_t *sn = Seat_Srv_FindByRowCol(sl, row, col);
		if (sn) {
			sn->data.status = (seat_status_t)status;
			Seat_Srv_Modify(&sn->data);
		} else {
			seat_t ns;
			memset(&ns, 0, sizeof ns);
			ns.roomID = roomID;
			ns.row = row;
			ns.column = col;
			ns.status = (seat_status_t)status;
			Seat_Srv_Add(&ns);
		}
		List_Destroy(sl, seat_node_t);
		send_json(client, 200, "{\"ok\":true,\"msg\":\"座位状态已更新\"}");
		return;
	}

	/* ---------- 剧目增改 ---------- */
	if (strcmp(method, "POST") == 0 && strcmp(path, "/api/play") == 0) {
		char act[32];
		if (!json_get_string(body, "action", act, sizeof act)) {
			send_json(client, 200, "{\"ok\":false,\"msg\":\"未知操作\"}");
			return;
		}
		if (strcmp(act, "add") == 0) {
			char name[64], typ[32];
			json_get_string(body, "name", name, sizeof name);
			json_get_string(body, "type", typ, sizeof typ);
			if (!typ[0]) strncpy(typ, "电影", sizeof typ - 1);
			int dur = 120;
			json_get_int(body, "duration", &dur);
			if (!name[0]) {
				send_json(client, 200, "{\"ok\":false,\"msg\":\"名称不能为空\"}");
				return;
			}
			play_t p;
			memset(&p, 0, sizeof p);
			strncpy(p.name, name, sizeof p.name - 1);
			strncpy(p.type, typ, sizeof p.type - 1);
			p.duration = dur;
			if (!Play_Srv_Add(&p)) {
				send_json(client, 200, "{\"ok\":false,\"msg\":\"添加失败\"}");
				return;
			}
			snprintf(g_resp, sizeof g_resp, "{\"ok\":true,\"msg\":\"添加成功\",\"id\":%d}", p.id);
			send_json(client, 200, g_resp);
			return;
		}
		if (strcmp(act, "update") == 0) {
			int pid = 0;
			json_get_int(body, "id", &pid);
			play_t p;
			if (!Play_Srv_FetchByID(pid, &p)) {
				send_json(client, 200, "{\"ok\":false,\"msg\":\"剧目不存在\"}");
				return;
			}
			char name[64], typ[32];
			if (json_get_string(body, "name", name, sizeof name)) strncpy(p.name, name, sizeof p.name - 1);
			if (json_get_string(body, "type", typ, sizeof typ)) strncpy(p.type, typ, sizeof p.type - 1);
			json_get_int(body, "duration", &p.duration);
			p.id = pid;
			Play_Srv_Modify(&p);
			send_json(client, 200, "{\"ok\":true,\"msg\":\"修改成功\"}");
			return;
		}
		send_json(client, 200, "{\"ok\":false,\"msg\":\"未知操作\"}");
		return;
	}

	/* ---------- 排期增改（影片 id 与影厅 id 须一致） ---------- */
	if (strcmp(method, "POST") == 0 && strcmp(path, "/api/schedule") == 0) {
		char act[32];
		if (!json_get_string(body, "action", act, sizeof act)) {
			send_json(client, 200, "{\"ok\":false,\"msg\":\"未知操作\"}");
			return;
		}
		if (strcmp(act, "add") == 0) {
			if (role != 0) {
				send_json(client, 200, "{\"ok\":false,\"msg\":\"仅管理员可添加排期\"}");
				return;
			}
			int playID = 0;
			json_get_int(body, "playID", &playID);
			int studioID = playID;
			json_get_int(body, "studioID", &studioID);
			if (studioID != playID) {
				send_json(client, 200, "{\"ok\":false,\"msg\":\"每部电影只能在同编号的影厅排期（影片ID与影厅ID须一致，如1号电影仅1号厅）\"}");
				return;
			}
			char date[24] = "2025-01-01", time[12] = "19:30";
			json_get_string(body, "date", date, sizeof date);
			json_get_string(body, "time", time, sizeof time);
			float price = 88;
			json_get_float(body, "price", &price);
			studio_t stu;
			if (!Studio_Srv_FetchByID(studioID, &stu)) {
				send_json(client, 200, "{\"ok\":false,\"msg\":\"影院不存在\"}");
				return;
			}
			seat_list_t sl;
			List_Init(sl, seat_node_t);
			int cnt = Seat_Srv_FetchByRoomID(sl, studioID);
			List_Destroy(sl, seat_node_t);
			if (cnt == 0) {
				List_Init(sl, seat_node_t);
				Seat_Srv_RoomInit(sl, studioID, stu.rowsCount, stu.colsCount);
				List_Destroy(sl, seat_node_t);
			}
			schedule_t sch;
			memset(&sch, 0, sizeof sch);
			sch.playID = playID;
			sch.studioID = studioID;
			strncpy(sch.date, date, sizeof sch.date - 1);
			strncpy(sch.time, time, sizeof sch.time - 1);
			sch.price = price;
			if (!Schedule_Srv_Add(&sch)) {
				send_json(client, 200, "{\"ok\":false,\"msg\":\"添加失败\"}");
				return;
			}
			snprintf(g_resp, sizeof g_resp, "{\"ok\":true,\"msg\":\"添加成功\",\"id\":%d}", sch.id);
			send_json(client, 200, g_resp);
			return;
		}
		if (strcmp(act, "update") == 0) {
			if (role != 0) {
				send_json(client, 200, "{\"ok\":false,\"msg\":\"仅管理员可修改排期\"}");
				return;
			}
			int sid = 0;
			json_get_int(body, "id", &sid);
			schedule_t s;
			if (!Schedule_Srv_FetchByID(sid, &s)) {
				send_json(client, 200, "{\"ok\":false,\"msg\":\"排期不存在\"}");
				return;
			}
			int playID = s.playID, studioID = s.studioID;
			if (json_find_value(body, "playID") && json_find_value(body, "studioID")) {
				json_get_int(body, "playID", &playID);
				json_get_int(body, "studioID", &studioID);
				if (playID != studioID) {
					send_json(client, 200, "{\"ok\":false,\"msg\":\"影片ID与影厅ID须一致\"}");
					return;
				}
				s.playID = playID;
				s.studioID = studioID;
			} else if (json_find_value(body, "playID")) {
				json_get_int(body, "playID", &playID);
				s.playID = playID;
				s.studioID = playID;
			} else if (json_find_value(body, "studioID")) {
				json_get_int(body, "studioID", &studioID);
				s.studioID = studioID;
				s.playID = studioID;
			}
			char date[24], time[12];
			if (json_get_string(body, "date", date, sizeof date)) strncpy(s.date, date, sizeof s.date - 1);
			if (json_get_string(body, "time", time, sizeof time)) strncpy(s.time, time, sizeof s.time - 1);
			json_get_float(body, "price", &s.price);
			s.id = sid;
			Schedule_Srv_Modify(&s);
			send_json(client, 200, "{\"ok\":true,\"msg\":\"修改成功\"}");
			return;
		}
		send_json(client, 200, "{\"ok\":false,\"msg\":\"未知操作\"}");
		return;
	}

	/* ---------- 账户增改（权限校验） ---------- */
	if (strcmp(method, "POST") == 0 && strcmp(path, "/api/account") == 0) {
		char act[32];
		if (!json_get_string(body, "action", act, sizeof act)) {
			send_json(client, 200, "{\"ok\":false,\"msg\":\"未知操作\"}");
			return;
		}
		if (strcmp(act, "add") == 0) {
			if (role != 0) {
				send_json(client, 200, "{\"ok\":false,\"msg\":\"仅管理员可添加账户\"}");
				return;
			}
			char user[64], pass[128];
			json_get_string(body, "username", user, sizeof user);
			json_get_string(body, "password", pass, sizeof pass);
			int rrole = 1;
			json_get_int(body, "role", &rrole);
			if (rrole == 0) {
				send_json(client, 200, "{\"ok\":false,\"msg\":\"不能添加管理员角色账户（仅内置管理员）\"}");
				return;
			}
			if (!user[0] || !pass[0]) {
				send_json(client, 200, "{\"ok\":false,\"msg\":\"用户名和密码不能为空\"}");
				return;
			}
			if (strcmp(user, BUILTIN_ADMIN_USER) == 0) {
				send_json(client, 200, "{\"ok\":false,\"msg\":\"该用户名为系统保留\"}");
				return;
			}
			account_t tmp;
			if (Account_Srv_FetchByUsername(user, &tmp)) {
				send_json(client, 200, "{\"ok\":false,\"msg\":\"用户名已存在\"}");
				return;
			}
			account_t a;
			memset(&a, 0, sizeof a);
			strncpy(a.username, user, sizeof a.username - 1);
			strncpy(a.password, pass, sizeof a.password - 1);
			a.role = (account_role_t)rrole;
			if (!Account_Srv_Add(&a)) {
				send_json(client, 200, "{\"ok\":false,\"msg\":\"添加失败\"}");
				return;
			}
			snprintf(g_resp, sizeof g_resp, "{\"ok\":true,\"msg\":\"添加成功\",\"id\":%d}", a.id);
			send_json(client, 200, g_resp);
			return;
		}
		if (strcmp(act, "update") == 0) {
			int aid = -1;
			json_get_int(body, "id", &aid);
			if (aid == BUILTIN_ADMIN_ID) {
				send_json(client, 200, "{\"ok\":false,\"msg\":\"内置管理员不在 data 中，无法通过此处修改\"}");
				return;
			}
			account_t a;
			if (!Account_Srv_FetchByID(aid, &a)) {
				send_json(client, 200, "{\"ok\":false,\"msg\":\"账户不存在\"}");
				return;
			}
			if (role != 0 && a.id != uid) {
				send_json(client, 200, "{\"ok\":false,\"msg\":\"仅能修改自己的账户\"}");
				return;
			}
			if (role != 0 && json_find_value(body, "role")) {
				send_json(client, 200, "{\"ok\":false,\"msg\":\"普通用户不能修改角色\"}");
				return;
			}
			char user[64];
			if (json_get_string(body, "username", user, sizeof user) && user[0]) {
				account_list_t L;
				List_Init(L, account_node_t);
				Account_Srv_FetchAll(L);
				account_node_t *p;
				int clash = 0;
				List_ForEach(L, p) {
					if (p->data.id != aid && strcmp(p->data.username, user) == 0) {
						clash = 1;
						break;
					}
				}
				List_Destroy(L, account_node_t);
				if (clash) {
					send_json(client, 200, "{\"ok\":false,\"msg\":\"用户名已存在\"}");
					return;
				}
				strncpy(a.username, user, sizeof a.username - 1);
			}
			char pass[128];
			if (json_get_string(body, "password", pass, sizeof pass) && pass[0])
				strncpy(a.password, pass, sizeof a.password - 1);
			if (role == 0 && json_find_value(body, "role")) {
				int nr = 1;
				json_get_int(body, "role", &nr);
				if (nr == 0) {
					send_json(client, 200, "{\"ok\":false,\"msg\":\"管理员仅内置账户，不能将 data 中用户改为管理员\"}");
					return;
				}
				a.role = (account_role_t)nr;
			}
			a.id = aid;
			Account_Srv_Modify(&a);
			send_json(client, 200, "{\"ok\":true,\"msg\":\"修改成功\"}");
			return;
		}
		send_json(client, 200, "{\"ok\":false,\"msg\":\"未知操作\"}");
		return;
	}

	send_json(client, 404, "{\"error\":\"not found\"}");
}

/*
 * 静态文件与 data.txt 均使用相对路径打开。
 * 若从快捷方式启动、或 exe 在 Debug 等子目录，默认工作目录会错，导致找不到 static 与 data.txt。
 * 做法：从 exe 路径逐级向上查找包含 index.html 的目录，并 SetCurrentDirectory 到该目录。
 */
static void set_workdir_to_project_root(void) {
	char path[MAX_PATH];
	DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) return;
	for (int depth = 0; depth < 10; depth++) {
		char *slash = strrchr(path, '\\');
		if (slash && slash > path) *slash = '\0';
		else break;
		char test[MAX_PATH + 24];
		snprintf(test, sizeof test, "%s\\index.html", path);
		if (GetFileAttributesA(test) != INVALID_FILE_ATTRIBUTES) {
			SetCurrentDirectoryA(path);
			return;
		}
	}
	/* 找不到 index.html 则退回到 exe 所在目录 */
	n = GetModuleFileNameA(NULL, path, MAX_PATH);
	if (n == 0 || n >= MAX_PATH) return;
	{
		char *slash = strrchr(path, '\\');
		if (slash && slash > path) {
			*slash = '\0';
			SetCurrentDirectoryA(path);
		}
	}
}

/* 程序入口：初始化 Winsock、绑定本机回环、循环 accept 处理 HTTP */
int main(void) {
	SetConsoleOutputCP(65001);
	set_workdir_to_project_root();
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
	SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (srv == INVALID_SOCKET) {
		WSACleanup();
		return 1;
	}
	int opt = 1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char *)&opt, sizeof opt);
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof addr);
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port = htons((unsigned short)HTTP_PORT);
	if (bind(srv, (struct sockaddr *)&addr, sizeof addr) != 0) {
		fprintf(stderr, "bind 127.0.0.1:%d failed\n", HTTP_PORT);
		closesocket(srv);
		WSACleanup();
		return 1;
	}
	listen(srv, SOMAXCONN);
	{
		char cwd[MAX_PATH];
		if (GetCurrentDirectoryA(sizeof cwd, cwd))
			printf("工作目录: %s\n", cwd);
	}
	printf("TTMS Web (C): http://127.0.0.1:%d/index.html\n", HTTP_PORT);
	printf("内置管理员（不写 data.txt）: %s / %s\n", BUILTIN_ADMIN_USER, BUILTIN_ADMIN_PASS);
	printf("静态图片请放在 static 目录，或与 index.html 同目录（兼容）\n");
	fflush(stdout);
	for (;;) {
		SOCKET c = accept(srv, NULL, NULL);
		if (c == INVALID_SOCKET) continue;
		handle_request(c);
		closesocket(c);
	}
}
