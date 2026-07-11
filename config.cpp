#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void config_default(ServerConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->port = 21;
    cfg->data_port_min = 30000;
    cfg->data_port_max = 30010;
    cfg->max_clients = 100;
    cfg->max_login_fails = 3;
    cfg->timeout = 300;
    cfg->daemon = 0;
    strcpy(cfg->listen_addr, "0.0.0.0");
    strcpy(cfg->root_dir, "/srv/ftp");
    strcpy(cfg->user_file, "/etc/ftpd/users.conf");
    strcpy(cfg->welcome_msg, "Welcome to Tiny FTP Server");
}

static void trim(char *s) {
    /* 去掉行首空白 */
    char *start = s;
    while (*start == ' ' || *start == '\t') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);

    /* 去掉行尾空白 */
    char *p = s + strlen(s) - 1;
    while (p >= s && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
        *p-- = '\0';
}

int config_load(ServerConfig *cfg, const char *path) {
    FILE *fp;
    char line[1024];
    char *eq, *key, *val;

    config_default(cfg);

    if (path == NULL) path = "/etc/ftpd/ftpd.conf";

    fp = fopen(path, "r");
    if (!fp) {
        fprintf(stderr, "[config] Cannot open config file: %s, using defaults\n", path);
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        trim(line);
        if (line[0] == '#' || line[0] == '\0') continue;

        eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        key = line;
        val = eq + 1;
        trim(key);
        trim(val);

        /* 去掉行尾注释（从第一个#开始截断） */
        char *pound = strchr(val, '#');
        if (pound) *pound = '\0';
        trim(val);

        if (strcasecmp(key, "port") == 0)
            cfg->port = atoi(val);
        else if (strcasecmp(key, "data_port_min") == 0)
            cfg->data_port_min = atoi(val);
        else if (strcasecmp(key, "data_port_max") == 0)
            cfg->data_port_max = atoi(val);
        else if (strcasecmp(key, "max_clients") == 0)
            cfg->max_clients = atoi(val);
        else if (strcasecmp(key, "max_login_fails") == 0)
            cfg->max_login_fails = atoi(val);
        else if (strcasecmp(key, "timeout") == 0)
            cfg->timeout = atoi(val);
        else if (strcasecmp(key, "listen_addr") == 0)
            strncpy(cfg->listen_addr, val, sizeof(cfg->listen_addr) - 1);
        else if (strcasecmp(key, "root_dir") == 0)
            strncpy(cfg->root_dir, val, sizeof(cfg->root_dir) - 1);
        else if (strcasecmp(key, "user_file") == 0)
            strncpy(cfg->user_file, val, sizeof(cfg->user_file) - 1);
        else if (strcasecmp(key, "welcome_msg") == 0)
            strncpy(cfg->welcome_msg, val, sizeof(cfg->welcome_msg) - 1);
        else
            fprintf(stderr, "[config] Unknown key: %s\n", key);
    }

    fclose(fp);
    return 0;
}

void config_print(const ServerConfig *cfg) {
    printf("=== Server Config ===\n");
    printf("  port          = %d\n", cfg->port);
    printf("  data_port_min = %d\n", cfg->data_port_min);
    printf("  data_port_max = %d\n", cfg->data_port_max);
    printf("  max_clients   = %d\n", cfg->max_clients);
    printf("  max_login_fails = %d\n", cfg->max_login_fails);
    printf("  timeout       = %d\n", cfg->timeout);
    printf("  listen_addr   = %s\n", cfg->listen_addr);
    printf("  root_dir      = %s\n", cfg->root_dir);
    printf("  user_file     = %s\n", cfg->user_file);
    printf("  welcome_msg   = %s\n", cfg->welcome_msg);
    printf("=====================\n");
}
