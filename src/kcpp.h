#ifndef STONE_KCP_H
#define STONE_KCP_H
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <vector>
#include <array>

#include <cstring>

#ifndef IWORDS_BIG_ENDIAN
#ifdef _BIG_ENDIAN_
#if _BIG_ENDIAN_
#define IWORDS_BIG_ENDIAN 1
#endif
#endif
#ifndef IWORDS_BIG_ENDIAN
#if defined(__hppa__) ||                                           \
    defined(__m68k__) || defined(mc68000) || defined(_M_M68K) ||   \
    (defined(__MIPS__) && defined(__MIPSEB__)) ||                  \
    defined(__ppc__) || defined(__POWERPC__) || defined(_M_PPC) || \
    defined(__sparc__) || defined(__powerpc__) ||                  \
    defined(__mc68000__) || defined(__s390x__) || defined(__s390__)
#define IWORDS_BIG_ENDIAN 1
#endif
#endif
#ifndef IWORDS_BIG_ENDIAN
#define IWORDS_BIG_ENDIAN 0
#endif
#endif

#ifndef IWORDS_MUST_ALIGN
#if defined(__i386__) || defined(__i386) || defined(_i386_)
#define IWORDS_MUST_ALIGN 0
#elif defined(_M_IX86) || defined(_X86_) || defined(__x86_64__)
#define IWORDS_MUST_ALIGN 0
#elif defined(__amd64) || defined(__amd64__)
#define IWORDS_MUST_ALIGN 0
#else
#define IWORDS_MUST_ALIGN 1
#endif
#endif

namespace stone
{

    const uint32_t KCP_RTO_NDL = 30;  // no delay min rto
    const uint32_t KCP_RTO_MIN = 100; // normal min rto
    const uint32_t KCP_RTO_DEF = 200;
    const uint32_t KCP_RTO_MAX = 60000;
    const uint32_t KCP_CMD_PUSH = 81; // cmd: push data
    const uint32_t KCP_CMD_ACK = 82;  // cmd: ack
    const uint32_t KCP_CMD_WASK = 83; // cmd: window probe (ask)
    const uint32_t KCP_CMD_WINS = 84; // cmd: window size (tell)
    const uint32_t KCP_ASK_SEND = 1;  // need to send KCP_CMD_WASK
    const uint32_t KCP_ASK_TELL = 2;  // need to send KCP_CMD_WINS
    const uint32_t KCP_WND_SND = 32;
    const uint32_t KCP_WND_RCV = 128; // must >= max fragment size
    const uint32_t KCP_MTU_DEF = 1400;
    const uint32_t KCP_ACK_FAST = 3;
    const uint32_t KCP_INTERVAL = 100;
    const uint32_t KCP_OVERHEAD = 24;
    const uint32_t KCP_DEADLINK = 20;
    const uint32_t KCP_THRESH_INIT = 2;
    const uint32_t KCP_THRESH_MIN = 2;
    const uint32_t KCP_PROBE_INIT = 7000;    // 7 secs to probe window size
    const uint32_t KCP_PROBE_LIMIT = 120000; // up to 120 secs to probe window
    const uint32_t KCP_FASTACK_LIMIT = 5;    // max times to trigger fastack

    struct kcpHeader
    {
        uint32_t conv;     // session id
        uint8_t cmd;       // control command
        uint8_t frg;       // fragment number
        uint16_t wnd;      // window size
        uint32_t ts;       // timestamp
        uint32_t sn;       // sequence number
        uint32_t una;      // unacknowledged sequence number
        uint32_t len;      // data length
        uint32_t resendts; // resend timestamp
        uint32_t rto;      // retransmission timeout
        uint32_t fastack;  // fast retransmit
        uint32_t xmit;     // transmit times
    };

    class kcpSeg
    {
    public:
        kcpSeg();
        kcpSeg(int size);
        kcpSeg(const kcpSeg &seg);
        kcpSeg &operator=(const kcpSeg &seg);
        kcpSeg(kcpSeg &&seg);
        kcpSeg &operator=(kcpSeg &&seg);
        ~kcpSeg();

        void parse_header(const char *data);

        char *copy_header2buf(char *buf);

        char *copy_data2buf(char *buf);
        int size();
        void set_data(const char *buf, int len);

        kcpHeader header_;
        char *data; //
    };

    class Kcpp;
    using outputCallBack = std::function<int(const char *buf, int len, Kcpp *kcp, void *user)>;

    class Kcpp
    {
    public:
        using kcpSegPtr = std::unique_ptr<kcpSeg>;
        using kcpSegList = std::list<kcpSegPtr>;
        using AckList = std::vector<std::array<uint32_t, 2>>;
        Kcpp(uint32_t conv, void *user);
        ~Kcpp();

        Kcpp(const Kcpp &) = delete;
        Kcpp &operator=(const Kcpp &) = delete;
        Kcpp(Kcpp &&) = delete;
        Kcpp &operator=(Kcpp &&) = delete;

    public:
        int send(const char *data, int len);
        int recv(char *buffer, int len);
        int input(const char *data, uint32_t size);
        void update(uint32_t current);
        int32_t check(uint32_t current);
        void flush();
        int peek_size();

        int wait_send_size();
        void no_delay(int nodelay, int interval, int resend, bool nocwnd);

        void set_wndsize(int sndwnd, int rcvwnd);
        void set_output(const outputCallBack &func);
        void set_interval(int interval);
        bool set_mtu(int mtu);
        void set_minrto(int minrto)
        {
            rx_minrto_ = minrto;
        }

        void set_stream(bool stream)
        {
            stream_ = stream;
        }

        void set_fastresend(int fastresend)
        {
            fastresend_ = fastresend;
        }


    private:
        void parse_fastack(uint32_t sn, uint32_t ts);

        void update_ack(int rtt);
        void update_probe();

        void check_data_repeat(kcpSegPtr newseg);
        void remove_ack(uint32_t sn);
        void remove_before_una(uint32_t una);

        int wnd_unused();
        void shrink_buf();
        void mv_buf_to_queue();
        void mv_queue_to_buf();

        

        int output(const char *data, int size);

        void flush_ack();
        void flush_window_probe();
        void flush_data();

        char *try_output(char *ptr);

    private:
        uint32_t conv_, mtu_, mss_;
        uint32_t snd_una_, snd_nxt_, rcv_nxt_;
        uint32_t ts_recent_, ts_lastack_, ssthresh_;
        int32_t rx_rttval_, rx_srtt_, rx_rto_, rx_minrto_;
        uint32_t snd_wnd_, rcv_wnd_, rmt_wnd_, cwnd_, probe_;
        uint32_t current_, interval_, ts_flush_, xmit_;
        uint32_t ts_probe_, probe_wait_;
        uint32_t dead_link_, incr_;
        int32_t nodelay_,fastresend_,fastlimit_;
        kcpSegList send_buf_;
        kcpSegList rcv_buf_;
        kcpSegList send_queue_;
        kcpSegList rcv_queue_;
        AckList acklist_;
        char *buffer_;
        void *user_;
        outputCallBack output_;
        bool nocwnd_, stream_, updated_, state_;
    };

}

#endif