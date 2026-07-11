#ifndef CONFIG_H
#define CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/* 服务器配置结构 */
typedef struct {
    int port;               /* 控制连接端口 (默认21) */
    int data_port_min;      /* PASV被动模式数据端口范围下限 */
    int data_port_max;      /* PASV被动模式数据端口范围上限 */
    int max_clients;        /* 最大并发客户端数 */
    int max_login_fails;    /* 最大登录失败次数 */
    int timeout;            /* 空闲超时(秒) */
    int daemon;             /* 是否后台运行 */
    char listen_addr[16];   /* 监听地址 */
    char root_dir[256];     /* FTP根目录 */
    char user_file[256];    /* 虚拟用户文件路径 */
    char welcome_msg[512];  /* 欢迎消息 */
} ServerConfig;

/* 加载配置文件，path为NULL则使用默认路径 /etc/ftpd.conf */
int config_load(ServerConfig *cfg, const char *path);

/* 设置默认配置 */
void config_default(ServerConfig *cfg);

/* 打印配置 */
void config_print(const ServerConfig *cfg);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */
