#ifndef AUTH_H
#define AUTH_H

#ifdef __cplusplus
extern "C" {
#endif

/* 用户信息 */
typedef struct {
    char username[64];
    char password[64];
    char home_dir[256];
} FtpUser;

/* 从用户文件中查找用户，成功返回0，失败返回-1 */
int auth_check(const char *user_file, const char *username,
               const char *password, FtpUser *user);

#ifdef __cplusplus
}
#endif

#endif /* AUTH_H */
