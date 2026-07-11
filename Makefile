# Makefile - Tiny FTP Server & Client
# 在 Linux 环境下使用 make 命令编译

CC = gcc
CXX = g++
CXXFLAGS = -std=c++11 -O2 -Wall -Wextra -Wno-unused-parameter
LDFLAGS =

# 服务端源文件（全部 .cpp 但使用 C 风格+C++混合）
SRV_SRCS = server.cpp session.cpp commands.cpp config.cpp auth.cpp \
           data_conn.cpp reactor.cpp
SRV_OBJS = $(SRV_SRCS:.cpp=.o)
SRV_TARGET = ftpd

# 客户端源文件
CLI_SRCS = client.cpp
CLI_OBJS = $(CLI_SRCS:.cpp=.o)
CLI_TARGET = ftp

# 默认目标
all: $(SRV_TARGET) $(CLI_TARGET)

# 服务端
$(SRV_TARGET): $(SRV_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# 客户端
$(CLI_TARGET): client.cpp
	$(CXX) $(CXXFLAGS) -o $@ $^

# 通用编译规则
%.o: %.cpp %.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# 没有头文件的cpp
server.o: server.cpp config.h auth.h reactor.h session.h commands.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

session.o: session.cpp session.h data_conn.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

# 清理
clean:
	rm -f $(SRV_TARGET) $(CLI_TARGET) *.o

# 安装（需要sudo）
install: $(SRV_TARGET)
	install -d /etc/ftpd
	install -m 644 ftpd.conf.example /etc/ftpd/ftpd.conf
	install -m 644 users.conf.example /etc/ftpd/users.conf
	install -m 755 $(SRV_TARGET) /usr/local/bin/$(SRV_TARGET)

# 运行
run: $(SRV_TARGET)
	sudo ./$(SRV_TARGET) -c ftpd.conf

rund: $(SRV_TARGET)
	sudo ./$(SRV_TARGET) -d -c ftpd.conf

# 伪目标
.PHONY: all clean install run rund
