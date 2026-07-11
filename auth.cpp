#include "auth.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/*
 * 用户文件格式（每行一个用户）:
 *   username:password:/home/ftp/path
 * 密码支持明文存储（开发/学习用途）
 */
int auth_check(const char *user_file, const char *username,
               const char *password, FtpUser *user) {
    FILE *fp;
    char line[1024];
    char *saveptr;
    char *file_user, *file_pass, *file_home;

    if (!user_file || !username || !password)
        return -1;

    fp = fopen(user_file, "r");
    if (!fp) {
        fprintf(stderr, "[auth] Cannot open user file: %s\n", user_file);
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        /* 去除换行 */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (len == 0 || line[0] == '#') continue;

        file_user = strtok_r(line, ":", &saveptr);
        if (!file_user) continue;
        file_pass = strtok_r(NULL, ":", &saveptr);
        if (!file_pass) continue;
        file_home = strtok_r(NULL, ":", &saveptr);
        if (!file_home) continue;

        if (strcmp(file_user, username) == 0 &&
            strcmp(file_pass, password) == 0) {
            strncpy(user->username, file_user, sizeof(user->username) - 1);
            strncpy(user->password, file_pass, sizeof(user->password) - 1);
            strncpy(user->home_dir, file_home, sizeof(user->home_dir) - 1);
            fclose(fp);
            return 0;  /* 认证成功 */
        }
    }

    fclose(fp);
    return -1;  /* 未找到或密码错误 */
}
