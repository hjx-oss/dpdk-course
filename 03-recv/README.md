# 第三集：接收报文

接收示例位于 [src/main.c](src/main.c)。本集只接收、查看并归还报文，不回包。

在仓库根目录完成环境配置后执行：

```bash
source ./env.sh
make -C 03-recv
make -C 03-recv check
```

测试在独立 user/network namespace 内创建 veth，检查三个 hello 帧、空载荷、二进制、连续收包、空闲退出和 mbuf 回收。测试通过后输出 `PASS: isolated veth test completed`。
