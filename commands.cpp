#include "commands.h"
#include "auth.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>
#include <pwd.h>
#include <grp.h>
#include <ctype.h>

/* ========== 辅助函数 ========== */

/* 获取文件类型字符 */
static char file_type_char(mode_t mode) {
    if (S_ISREG(mode)) return '-';
    if (S_ISDIR(mode)) return 'd';
    if (S_ISLNK(mode)) return 'l';
    if (S_ISCHR(mode)) return 'c';
    if (S_ISBLK(mode)) return 'b';
    if (S_ISFIFO(mode)) return 'p';
    if (S_ISSOCK(mode)) return 's';
    return '?';
}

/* 获取权限字符串（rwxr-xr-x格式） */
static void perm_string(mode_t mode, char *buf) {
    buf[0] = (mode & S_IRUSR) ? 'r' : '-';
    buf[1] = (mode & S_IWUSR) ? 'w' : '-';
    buf[2] = (mode & S_IXUSR) ? 'x' : '-';
    buf[3] = (mode & S_IRGRP) ? 'r' : '-';
    buf[4] = (mode & S_IWGRP) ? 'w' : '-';
    buf[5] = (mode & S_IXGRP) ? 'x' : '-';
    buf[6] = (mode & S_IROTH) ? 'r' : '-';
    buf[7] = (mode & S_IWOTH) ? 'w' : '-';
    buf[8] = (mode & S_IXOTH) ? 'x' : '-';
    buf[9] = '\0';
}

/* 发送目录列表到数据连接 */
static int send_list_data(FtpSession *s, const char *path, int long_list) {
    DIR *dir;
    struct dirent *entry;
    struct stat st;
    char fullpath[1024];
    char buf[4096];
    int total = 0;

    dir = opendir(path);
    if (!dir) {
        session_reply(s, "550 Failed to open directory.\r\n");
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;  /* 跳过隐藏文件 */

        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, entry->d_name);

        if (long_list) {
            if (stat(fullpath, &st) < 0) continue;

            char perm[10];
            perm_string(st.st_mode, perm);
            char timebuf[64];
            struct tm *tm = localtime(&st.st_mtime);
            strftime(timebuf, sizeof(timebuf), "%b %e %H:%M", tm);

            snprintf(buf, sizeof(buf), "%c%s %3lu %8lld %s %s\r\n",
                     file_type_char(st.st_mode),
                     perm,
                     (unsigned long)st.st_nlink,
                     (long long)st.st_size,
                     timebuf,
                     entry->d_name);
        } else {
            snprintf(buf, sizeof(buf), "%s\r\n", entry->d_name);
        }

        int len = strlen(buf);
        int fd = s->data_conn.fd;
        if (fd >= 0) {
            ssize_t n = write(fd, buf, len);
            if (n < 0) break;
            total += n;
        }
    }

    closedir(dir);
    return total;
}

/* 打开文件用于RETR传输 */
static int open_file_for_retr(FtpSession *s, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        session_reply(s, "550 Failed to open file.\r\n");
        return -1;
    }

    /* 断点续传：跳过指定字节 */
    if (s->data_conn.restart_pos > 0) {
        off_t r = lseek(fd, s->data_conn.restart_pos, SEEK_SET);
        if (r < 0) {
            close(fd);
            session_reply(s, "550 Cannot resume transfer.\r\n");
            return -1;
        }
    }

    s->xfer_type = XFER_RETR;
    s->xfer_fd = fd;
    s->xfer_offset = s->data_conn.restart_pos;
    return 0;
}

/* ========== 命令实现 ========== */

int cmd_USER(FtpSession *s, const char *arg) {
    if (!arg || arg[0] == '\0') {
        session_reply(s, "501 Syntax error in parameters.\r\n");
        return 0;
    }
    strncpy(s->username, arg, sizeof(s->username) - 1);
    s->state = ST_WAIT_PASS;
    session_reply(s, "331 User name okay, need password.\r\n");
    return 0;
}

int cmd_PASS(FtpSession *s, const char *arg) {
    /* 加载用户认证模块验证 */
    FtpUser user;
    /* 从配置文件获取user_file路径 - 通过外部配置传入 */
    extern char g_user_file[256];

    if (auth_check(g_user_file, s->username, arg ? arg : "", &user) == 0) {
        /* 认证成功 */
        strcpy(s->home_dir, user.home_dir);
        s->current_dir[0] = '\0';
        s->state = ST_LOGGED_IN;
        session_reply(s, "230 User logged in, proceed.\r\n");
        printf("[auth] User '%s' logged in, home=%s\n", s->username, s->home_dir);
        return 0;
    }

    s->login_fails++;
    if (s->login_fails >= 3) {
        session_reply(s, "421 Too many login failures. Disconnecting.\r\n");
        return -1;
    }
    session_reply(s, "530 Login incorrect.\r\n");
    return 0;
}

int cmd_QUIT(FtpSession *s, const char *arg) {
    session_reply(s, "221 Goodbye.\r\n");
    return -1;
}

int cmd_PWD(FtpSession *s, const char *arg) {
    if (s->current_dir[0] == '\0')
        session_reply(s, "257 \"/\" is the current directory.\r\n");
    else
        session_reply(s, "257 \"%s\" is the current directory.\r\n", s->current_dir);
    return 0;
}

int cmd_CWD(FtpSession *s, const char *arg) {
    char abs_path[1024];
    struct stat st;

    if (!arg || arg[0] == '\0') {
        s->current_dir[0] = '\0';
        session_reply(s, "250 Directory successfully changed.\r\n");
        return 0;
    }

    session_resolve_path(s, arg, abs_path, sizeof(abs_path));

    if (!session_path_safe(s, abs_path)) {
        session_reply(s, "550 Access denied.\r\n");
        return 0;
    }

    if (stat(abs_path, &st) < 0 || !S_ISDIR(st.st_mode)) {
        session_reply(s, "550 Failed to change directory.\r\n");
        return 0;
    }

    /* 更新current_dir为相对home_dir的路径 */
    if (strcmp(abs_path, s->home_dir) == 0) {
        s->current_dir[0] = '\0';
    } else {
        snprintf(s->current_dir, sizeof(s->current_dir), "%s",
                 abs_path + strlen(s->home_dir));
    }

    session_reply(s, "250 Directory successfully changed.\r\n");
    return 0;
}

int cmd_CDUP(FtpSession *s, const char *arg) {
    return cmd_CWD(s, "..");
}

int cmd_MKD(FtpSession *s, const char *arg) {
    char abs_path[1024];

    if (!arg || arg[0] == '\0') {
        session_reply(s, "501 Syntax error in parameters.\r\n");
        return 0;
    }

    session_resolve_path(s, arg, abs_path, sizeof(abs_path));
    if (!session_path_safe(s, abs_path)) {
        session_reply(s, "550 Access denied.\r\n");
        return 0;
    }

    if (mkdir(abs_path, 0755) < 0) {
        session_reply(s, "550 Failed to create directory.\r\n");
        return 0;
    }

    session_reply(s, "257 \"/%s\" directory created.\r\n", arg);
    return 0;
}

int cmd_RMD(FtpSession *s, const char *arg) {
    char abs_path[1024];

    if (!arg || arg[0] == '\0') {
        session_reply(s, "501 Syntax error in parameters.\r\n");
        return 0;
    }

    session_resolve_path(s, arg, abs_path, sizeof(abs_path));
    if (!session_path_safe(s, abs_path)) {
        session_reply(s, "550 Access denied.\r\n");
        return 0;
    }

    if (rmdir(abs_path) < 0) {
        session_reply(s, "550 Failed to remove directory.\r\n");
        return 0;
    }

    session_reply(s, "250 Directory removed.\r\n");
    return 0;
}

int cmd_DELE(FtpSession *s, const char *arg) {
    char abs_path[1024];

    if (!arg || arg[0] == '\0') {
        session_reply(s, "501 Syntax error in parameters.\r\n");
        return 0;
    }

    session_resolve_path(s, arg, abs_path, sizeof(abs_path));
    if (!session_path_safe(s, abs_path)) {
        session_reply(s, "550 Access denied.\r\n");
        return 0;
    }

    if (unlink(abs_path) < 0) {
        session_reply(s, "550 Failed to delete file.\r\n");
        return 0;
    }

    session_reply(s, "250 File deleted.\r\n");
    return 0;
}

int cmd_RNFR(FtpSession *s, const char *arg) {
    char abs_path[1024];

    if (!arg || arg[0] == '\0') {
        session_reply(s, "501 Syntax error in parameters.\r\n");
        return 0;
    }

    session_resolve_path(s, arg, abs_path, sizeof(abs_path));
    if (!session_path_safe(s, abs_path)) {
        session_reply(s, "550 Access denied.\r\n");
        return 0;
    }

    if (access(abs_path, F_OK) < 0) {
        session_reply(s, "550 File not found.\r\n");
        return 0;
    }

    strncpy(s->rename_from, abs_path, sizeof(s->rename_from) - 1);
    s->rename_pending = 1;
    session_reply(s, "350 File exists, proceed with RNTO.\r\n");
    return 0;
}

int cmd_RNTO(FtpSession *s, const char *arg) {
    char abs_path[1024];

    if (!s->rename_pending) {
        session_reply(s, "503 Bad sequence of commands.\r\n");
        return 0;
    }

    if (!arg || arg[0] == '\0') {
        session_reply(s, "501 Syntax error in parameters.\r\n");
        return 0;
    }

    session_resolve_path(s, arg, abs_path, sizeof(abs_path));
    if (!session_path_safe(s, abs_path)) {
        session_reply(s, "550 Access denied.\r\n");
        return 0;
    }

    if (rename(s->rename_from, abs_path) < 0) {
        session_reply(s, "550 Failed to rename file.\r\n");
    } else {
        session_reply(s, "250 File renamed successfully.\r\n");
    }

    s->rename_pending = 0;
    return 0;
}

int cmd_SIZE(FtpSession *s, const char *arg) {
    char abs_path[1024];
    struct stat st;

    if (!arg || arg[0] == '\0') {
        session_reply(s, "501 Syntax error in parameters.\r\n");
        return 0;
    }

    session_resolve_path(s, arg, abs_path, sizeof(abs_path));
    if (!session_path_safe(s, abs_path)) {
        session_reply(s, "550 Access denied.\r\n");
        return 0;
    }

    if (stat(abs_path, &st) < 0) {
        session_reply(s, "550 File not found.\r\n");
        return 0;
    }

    session_reply(s, "213 %lld\r\n", (long long)st.st_size);
    return 0;
}

int cmd_TYPE(FtpSession *s, const char *arg) {
    if (!arg || arg[0] == '\0') {
        session_reply(s, "501 Syntax error.\r\n");
        return 0;
    }

    char type = arg[0];
    if (type == 'A' || type == 'a') {
        s->xfer_mode = TYPE_ASCII;
        session_reply(s, "200 Type set to ASCII.\r\n");
    } else if (type == 'I' || type == 'i') {
        s->xfer_mode = TYPE_BINARY;
        session_reply(s, "200 Type set to Binary.\r\n");
    } else {
        session_reply(s, "504 Unsupported type.\r\n");
    }
    return 0;
}

int cmd_REST(FtpSession *s, const char *arg) {
    if (!arg || arg[0] == '\0') {
        session_reply(s, "501 Syntax error.\r\n");
        return 0;
    }

    off_t pos = (off_t)atoll(arg);
    if (pos < 0) {
        session_reply(s, "501 Invalid offset.\r\n");
        return 0;
    }

    s->data_conn.restart_pos = pos;
    session_reply(s, "350 Restart position accepted (%lld).\r\n", (long long)pos);
    return 0;
}

int cmd_PASV(FtpSession *s, const char *arg) {
    extern ServerConfig g_config;
    int port;

    /* 先关闭可能存在的旧数据连接 */
    data_conn_close(&s->data_conn);

    port = data_conn_setup_pasv(&s->data_conn,
                                 g_config.data_port_min,
                                 g_config.data_port_max);
    if (port < 0) {
        session_reply(s, "425 Cannot open passive connection.\r\n");
        return 0;
    }

    /* 发送PASV响应：227 Entering Passive Mode (h1,h2,h3,h4,p1,p2) */
    char port_str[64];
    data_conn_format_port(&s->ctrl_addr, port, port_str, sizeof(port_str));
    session_reply(s, "227 Entering Passive Mode (%s).\r\n", port_str);
    return 0;
}

int cmd_PORT(FtpSession *s, const char *arg) {
    if (!arg || arg[0] == '\0') {
        session_reply(s, "501 Syntax error.\r\n");
        return 0;
    }

    data_conn_close(&s->data_conn);

    if (data_conn_parse_port(&s->data_conn, arg) < 0) {
        session_reply(s, "501 Bad port argument.\r\n");
        return 0;
    }

    session_reply(s, "200 PORT command successful.\r\n");
    return 0;
}

int cmd_LIST(FtpSession *s, const char *arg) {
    char abs_path[1024];
    struct stat st;

    /* 确保数据连接已建立 */
    if (s->data_conn.mode == DATA_NONE) {
        session_reply(s, "425 Use PORT or PASV first.\r\n");
        return 0;
    }

    /* 解析路径参数 */
    if (arg && arg[0] != '\0') {
        session_resolve_path(s, arg, abs_path, sizeof(abs_path));
        if (!session_path_safe(s, abs_path)) {
            session_reply(s, "550 Access denied.\r\n");
            data_conn_close(&s->data_conn);
            return 0;
        }
    } else {
        session_get_abs_path(s, abs_path, sizeof(abs_path));
    }

    if (stat(abs_path, &st) < 0) {
        session_reply(s, "550 File not found.\r\n");
        data_conn_close(&s->data_conn);
        return 0;
    }

    /* 处理PASV/PORT连接 */
    if (s->data_conn.mode == DATA_PASV) {
        if (data_conn_accept_pasv(&s->data_conn) < 0) {
            session_reply(s, "425 Cannot open data connection.\r\n");
            data_conn_close(&s->data_conn);
            return 0;
        }
    } else if (s->data_conn.mode == DATA_PORT) {
        if (data_conn_connect_port(&s->data_conn) < 0) {
            session_reply(s, "425 Cannot open data connection.\r\n");
            data_conn_close(&s->data_conn);
            return 0;
        }
    }

    session_reply(s, "150 Opening data connection for directory listing.\r\n");

    if (S_ISDIR(st.st_mode)) {
        send_list_data(s, abs_path, 1);  /* 长格式 */
    } else {
        /* LIST也可以用于单个文件 */
        struct stat st;
        stat(abs_path, &st);
        char perm[10];
        perm_string(st.st_mode, perm);
        char timebuf[64];
        struct tm *tm = localtime(&st.st_mtime);
        strftime(timebuf, sizeof(timebuf), "%b %e %H:%M", tm);
        char buf[1024];
        snprintf(buf, sizeof(buf), "%c%s %3lu %8lld %s %s\r\n",
                 file_type_char(st.st_mode), perm,
                 (unsigned long)st.st_nlink,
                 (long long)st.st_size,
                 timebuf, arg ? arg : "");
        if (s->data_conn.fd >= 0)
            write(s->data_conn.fd, buf, strlen(buf));
    }

    data_conn_close(&s->data_conn);
    session_reply(s, "226 Directory send OK.\r\n");
    return 0;
}

int cmd_NLST(FtpSession *s, const char *arg) {
    /* NLST同LIST但只输出名字 */
    char abs_path[1024];

    if (s->data_conn.mode == DATA_NONE) {
        session_reply(s, "425 Use PORT or PASV first.\r\n");
        return 0;
    }

    if (arg && arg[0] != '\0') {
        session_resolve_path(s, arg, abs_path, sizeof(abs_path));
        if (!session_path_safe(s, abs_path)) {
            session_reply(s, "550 Access denied.\r\n");
            data_conn_close(&s->data_conn);
            return 0;
        }
    } else {
        session_get_abs_path(s, abs_path, sizeof(abs_path));
    }

    if (s->data_conn.mode == DATA_PASV)
        data_conn_accept_pasv(&s->data_conn);
    else if (s->data_conn.mode == DATA_PORT)
        data_conn_connect_port(&s->data_conn);

    session_reply(s, "150 Opening data connection.\r\n");
    send_list_data(s, abs_path, 0);  /* 只有名字 */
    data_conn_close(&s->data_conn);
    session_reply(s, "226 Directory send OK.\r\n");
    return 0;
}

int cmd_RETR(FtpSession *s, const char *arg) {
    char abs_path[1024];
    struct stat st;

    if (!arg || arg[0] == '\0') {
        session_reply(s, "501 Syntax error.\r\n");
        return 0;
    }

    if (s->data_conn.mode == DATA_NONE) {
        session_reply(s, "425 Use PORT or PASV first.\r\n");
        return 0;
    }

    session_resolve_path(s, arg, abs_path, sizeof(abs_path));
    if (!session_path_safe(s, abs_path)) {
        session_reply(s, "550 Access denied.\r\n");
        data_conn_close(&s->data_conn);
        return 0;
    }

    if (stat(abs_path, &st) < 0 || !S_ISREG(st.st_mode)) {
        session_reply(s, "550 File not found.\r\n");
        data_conn_close(&s->data_conn);
        return 0;
    }

    /* 建立数据连接 */
    if (s->data_conn.mode == DATA_PASV) {
        if (data_conn_accept_pasv(&s->data_conn) < 0) {
            session_reply(s, "425 Cannot open data connection.\r\n");
            data_conn_close(&s->data_conn);
            return 0;
        }
    } else if (s->data_conn.mode == DATA_PORT) {
        if (data_conn_connect_port(&s->data_conn) < 0) {
            session_reply(s, "425 Cannot open data connection.\r\n");
            data_conn_close(&s->data_conn);
            return 0;
        }
    }

    if (open_file_for_retr(s, abs_path) < 0) {
        data_conn_close(&s->data_conn);
        return 0;
    }

    /* 发送文件内容 */
    char buf[8192];
    ssize_t n;
    off_t total = 0;

    while ((n = read(s->xfer_fd, buf, sizeof(buf))) > 0) {
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(s->data_conn.fd, buf + written, n - written);
            if (w < 0) {
                if (errno == EAGAIN) continue;
                goto done;
            }
            written += w;
            total += w;
        }
    }

done:
    close(s->xfer_fd);
    s->xfer_fd = -1;
    s->xfer_type = XFER_NONE;
    s->xfer_offset = 0;
    s->data_conn.restart_pos = 0;

    session_reply(s, "150 Opening data connection for file download.\r\n");
    data_conn_close(&s->data_conn);
    session_reply(s, "226 Transfer complete. (%lld bytes)\r\n", (long long)total);
    return 0;
}

int cmd_STOR(FtpSession *s, const char *arg) {
    char abs_path[1024];

    if (!arg || arg[0] == '\0') {
        session_reply(s, "501 Syntax error.\r\n");
        return 0;
    }

    if (s->data_conn.mode == DATA_NONE) {
        session_reply(s, "425 Use PORT or PASV first.\r\n");
        return 0;
    }

    session_resolve_path(s, arg, abs_path, sizeof(abs_path));
    if (!session_path_safe(s, abs_path)) {
        session_reply(s, "550 Access denied.\r\n");
        data_conn_close(&s->data_conn);
        return 0;
    }

    /* 建立数据连接 */
    if (s->data_conn.mode == DATA_PASV) {
        if (data_conn_accept_pasv(&s->data_conn) < 0) {
            session_reply(s, "425 Cannot open data connection.\r\n");
            return 0;
        }
    } else if (s->data_conn.mode == DATA_PORT) {
        if (data_conn_connect_port(&s->data_conn) < 0) {
            session_reply(s, "425 Cannot open data connection.\r\n");
            return 0;
        }
    }

    /* 打开文件（支持断点续传） */
    int flags = O_WRONLY | O_CREAT;
    if (s->data_conn.restart_pos > 0) {
        flags |= O_APPEND;
    } else {
        flags |= O_TRUNC;
    }

    int filefd = open(abs_path, flags, 0666);
    if (filefd < 0) {
        session_reply(s, "550 Cannot create file.\r\n");
        data_conn_close(&s->data_conn);
        return 0;
    }

    if (s->data_conn.restart_pos > 0) {
        lseek(filefd, s->data_conn.restart_pos, SEEK_SET);
    }

    /* 接收数据并写入 */
    char buf[8192];
    ssize_t n;
    off_t total = 0;

    session_reply(s, "150 Opening data connection for file upload.\r\n");

    while ((n = read(s->data_conn.fd, buf, sizeof(buf))) > 0) {
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(filefd, buf + written, n - written);
            if (w < 0) {
                if (errno == EAGAIN) { continue; }
                goto stor_done;
            }
            written += w;
            total += w;
        }
    }

stor_done:
    close(filefd);
    s->xfer_type = XFER_NONE;
    s->data_conn.restart_pos = 0;

    data_conn_close(&s->data_conn);
    session_reply(s, "226 Transfer complete. (%lld bytes)\r\n", (long long)total);
    return 0;
}

int cmd_SYST(FtpSession *s, const char *arg) {
    session_reply(s, "215 UNIX Type: L8\r\n");
    return 0;
}

int cmd_NOOP(FtpSession *s, const char *arg) {
    session_reply(s, "200 NOOP OK.\r\n");
    return 0;
}

int cmd_FEAT(FtpSession *s, const char *arg) {
    session_reply(s, "211-Features:\r\n"
                      " PASV\r\n"
                      " PORT\r\n"
                      " REST STREAM\r\n"
                      " SIZE\r\n"
                      "211 End.\r\n");
    return 0;
}

int cmd_HELP(FtpSession *s, const char *arg) {
    session_reply(s, "214-The following commands are recognized:\r\n"
                      " USER PASS QUIT CWD CDUP PWD\r\n"
                      " LIST NLST RETR STOR TYPE\r\n"
                      " PASV PORT REST SIZE SYST\r\n"
                      " MKD RMD DELE RNFR RNTO\r\n"
                      " NOOP FEAT HELP\r\n"
                      "214 Help OK.\r\n");
    return 0;
}

int cmd_UNKNOWN(FtpSession *s, const char *arg) {
    session_reply(s, "500 Unknown command.\r\n");
    return 0;
}

/* ========== 命令分发表 ========== */

CommandEntry cmd_table[] = {
    { "USER",  cmd_USER,  0 },
    { "PASS",  cmd_PASS,  0 },
    { "QUIT",  cmd_QUIT,  0 },
    { "PWD",   cmd_PWD,   1 },
    { "CWD",   cmd_CWD,   1 },
    { "CDUP",  cmd_CDUP,  1 },
    { "MKD",   cmd_MKD,   1 },
    { "RMD",   cmd_RMD,   1 },
    { "DELE",  cmd_DELE,  1 },
    { "RNFR",  cmd_RNFR,  1 },
    { "RNTO",  cmd_RNTO,  1 },
    { "SIZE",  cmd_SIZE,  1 },
    { "LIST",  cmd_LIST,  1 },
    { "NLST",  cmd_NLST,  1 },
    { "TYPE",  cmd_TYPE,  1 },
    { "REST",  cmd_REST,  1 },
    { "PASV",  cmd_PASV,  1 },
    { "PORT",  cmd_PORT,  1 },
    { "RETR",  cmd_RETR,  1 },
    { "STOR",  cmd_STOR,  1 },
    { "SYST",  cmd_SYST,  0 },
    { "NOOP",  cmd_NOOP,  0 },
    { "FEAT",  cmd_FEAT,  0 },
    { "HELP",  cmd_HELP,  0 },
    { NULL,    NULL,      0 }   /* 结束标志 */
};

/* 命令分发：解析行并执行命令 */
int cmd_dispatch(FtpSession *s, const char *line) {
    char cmd[64];
    const char *arg;
    int i;

    /* 解析命令和参数 */
    const char *space = strchr(line, ' ');
    if (space) {
        size_t cmd_len = space - line;
        if (cmd_len >= sizeof(cmd)) cmd_len = sizeof(cmd) - 1;
        memcpy(cmd, line, cmd_len);
        cmd[cmd_len] = '\0';
        arg = space + 1;
        /* 跳过参数前的空白 */
        while (*arg == ' ') arg++;
        if (*arg == '\0') arg = NULL;
    } else {
        strncpy(cmd, line, sizeof(cmd) - 1);
        arg = NULL;
    }

    /* 转为大写 */
    for (i = 0; cmd[i]; i++)
        cmd[i] = toupper((unsigned char)cmd[i]);

    /* 查找命令 */
    for (i = 0; cmd_table[i].name; i++) {
        if (strcmp(cmd, cmd_table[i].name) == 0) {
            if (cmd_table[i].need_login && s->state != ST_LOGGED_IN) {
                session_reply(s, "530 Please login with USER and PASS.\r\n");
                return 0;
            }
            return cmd_table[i].handler(s, arg);
        }
    }

    return cmd_UNKNOWN(s, arg);
}
