// 第 2 章：一个包在 D+/D- 上是什么样子。
//
// 依次演示：字节→比特（低位先发）、NRZI 编码、位填充、SYNC 和 EOP，
// 最后把一个真实的 ACK 包和 SETUP 令牌画成波形，再解码回来。
#include <cstdio>

#include "miniusb/signal.hpp"

using namespace musb;

static void title(const char* t)
{
    std::printf("\n========== %s ==========\n", t);
}

int main()
{
    title("第 1 步：字节变比特，最低位先发");
    Bytes b{0x2D};
    std::printf("0x2D = 0b00101101，上线顺序（bit0 先）：%s\n", to_string(bytes_to_bits(b)).c_str());

    title("第 2 步：NRZI —— 0 就翻转，1 就不变");
    Bits zeros{0, 0, 0, 0, 0, 0, 0, 0};
    Bits ones{1, 1, 1, 1, 1, 1, 1, 1};
    std::printf("8 个 0：%s   <- 每位都跳变，接收方很容易对准时钟\n",
                to_string(nrzi_encode(zeros)).c_str());
    std::printf("8 个 1：%s   <- 一直不变，接收方数不清有几位\n",
                to_string(nrzi_encode(ones)).c_str());

    title("第 3 步：位填充 —— 连续 6 个 1 后强行插入一个 0");
    Bits ff = bytes_to_bits({0xFF, 0xFF});
    Bits stuffed = bit_stuff(ff);
    std::printf("原始 16 位：%s\n", to_string(ff).c_str());
    std::printf("填充后 %zu 位：%s   <- 第 7、14 位是插进去的 0\n", stuffed.size(),
                to_string(stuffed).c_str());
    std::printf("NRZI 之后：%s   <- 最多 7 位就有一次跳变\n",
                to_string(nrzi_encode(stuffed)).c_str());
    Bits bad{1, 1, 1, 1, 1, 1, 1};
    std::printf("接收方看到 7 个连续的 1：%s\n",
                bit_unstuff(bad) ? "正常" : "位填充错误，整个包丢弃");

    title("第 4 步：一个完整的 ACK 包（PID = 0xD2）");
    Lines ack = to_wire({0xD2});
    std::printf("SYNC(KJKJKJKK) + PID(8 位) + EOP(SE0 SE0 J)：\n%s\n", to_string(ack).c_str());
    std::printf("%s", waveform(ack).c_str());
    std::printf("全速每位 1/12MHz = 83.3 ns，这个包在线上占 %zu 位 = %.2f us\n", ack.size(),
                ack.size() / 12.0);

    title("第 5 步：SETUP 令牌包 2D 00 10（地址 0、端点 0）");
    Lines setup = to_wire({0x2D, 0x00, 0x10});
    std::printf("%s", waveform(setup).c_str());

    title("第 6 步：接收方把波形解码回字节");
    auto back = from_wire(setup);
    std::printf("解码结果：%s\n", back ? hex(*back).c_str() : "失败");

    Lines noisy = setup;
    noisy[12] = flip(noisy[12]);   // 噪声让第 12 位的电平翻了
    auto back2 = from_wire(noisy);
    std::printf("第 12 位被干扰后再解码：%s\n", back2 ? hex(*back2).c_str() : "失败");
    std::printf("（字节还能解出来，但已经不对了 —— 下一章用 PID 校验和 CRC 把它揪出来）\n");
    return 0;
}
