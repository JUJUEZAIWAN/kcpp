#include "kcpp.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <arpa/inet.h>
using namespace stone;

static inline long _itimediff(uint32_t later, uint32_t earlier)
{
    return static_cast<long>(later) - static_cast<long>(earlier);
}

kcpSeg::kcpSeg() : data(nullptr)
{
    memset(&header_, 0, sizeof(kcpHeader));
}

kcpSeg::kcpSeg(int size)
    : data(new char[size])
{
    memset(&header_, 0, sizeof(kcpHeader));
}

kcpSeg::~kcpSeg()
{
    delete[] data;
}

kcpSeg::kcpSeg(const kcpSeg &seg)
{
    header_ = seg.header_;
    data = new char[seg.header_.len];
}
kcpSeg &kcpSeg::operator=(const kcpSeg &seg)
{
    header_ = seg.header_;
    data = new char[seg.header_.len];
    return *this;
}
kcpSeg::kcpSeg(kcpSeg &&seg)
{
    header_ = seg.header_;
    data = seg.data;
    seg.data = nullptr;
}
kcpSeg &kcpSeg::operator=(kcpSeg &&seg)
{
    header_ = seg.header_;
    data = seg.data;
    seg.data = nullptr;
    return *this;
}

void kcpSeg::parse_header(const char *data)
{
    memcpy(&header_, data, KCP_OVERHEAD);
#if IWORDS_BIG_ENDIAN || IWORDS_MUST_ALIGN
    header_.conv = ntohl(header_.conv);
    header_.cmd = ntohl(header_.cmd);
    header_.frg = ntohl(header_.frg);
    header_.wnd = ntohl(header_.wnd);
    header_.ts = ntohl(header_.ts);
    header_.sn = ntohl(header_.sn);
    header_.una = ntohl(header_.una);
    header_.len = ntohl(header_.len);
#endif
}

char *kcpSeg::copy_header2buf(char *buf)
{
    memcpy(buf, &header_, KCP_OVERHEAD); // don't use sizeof(kcpHeader) here
    return buf + KCP_OVERHEAD;
}

char *kcpSeg::copy_data2buf(char *buf)
{
    memcpy(buf, data, header_.len);
    return buf + header_.len;
}

int kcpSeg::size()
{
    return sizeof(kcpHeader) + header_.len;
}

void kcpSeg::set_data(const char *buf, int len)
{
    memcpy(data, buf, len);
}

Kcpp::Kcpp(uint32_t conv, void *user)
    : conv_(conv), mtu_(KCP_MTU_DEF), mss_(mtu_ - KCP_OVERHEAD),
      snd_una_(0), snd_nxt_(0), rcv_nxt_(0), ts_recent_(0), ts_lastack_(0), ssthresh_(KCP_THRESH_INIT),
      rx_rttval_(0), rx_srtt_(0), rx_rto_(KCP_RTO_DEF), rx_minrto_(KCP_RTO_MIN),
      snd_wnd_(KCP_WND_SND), rcv_wnd_(KCP_WND_RCV), rmt_wnd_(KCP_WND_RCV), cwnd_(0), probe_(0),
      current_(0), interval_(KCP_INTERVAL), ts_flush_(KCP_INTERVAL), xmit_(0),
      ts_probe_(0), probe_wait_(0), dead_link_(KCP_DEADLINK), incr_(0),
      nodelay_(0), fastresend_(0), fastlimit_(KCP_FASTACK_LIMIT),
      buffer_(new char[(mtu_ + KCP_OVERHEAD) * 3]),
      nocwnd_(false), stream_(false), updated_(false), state_(false), user_(user), output_(nullptr)
{
}

Kcpp::~Kcpp()
{
    delete[] buffer_;
}

void Kcpp::set_output(const outputCallBack &func)
{
    output_ = func;
}

bool Kcpp::set_mtu(int mtu)
{
    if (mtu < 50 || mtu < static_cast<int>(KCP_OVERHEAD))
    {
        return false;
    }
    char *buffer = new char[(mtu + KCP_OVERHEAD) * 3];

    delete[] buffer_;
    buffer_ = buffer;
    mtu_ = mtu;
    mss_ = mtu_ - KCP_OVERHEAD;
    return true;
}

void Kcpp::set_interval(int interval)
{
    if (interval > 5000)
    {
        interval = 5000;
    }
    else if (interval < 10)
    {
        interval = 10;
    }
    interval_ = interval;
}

void Kcpp::no_delay(int nodelay, int interval, int resend, bool nocwnd)
{
    if (nodelay >= 0)
    {
        nodelay_ = nodelay;
        if (nodelay_ != 0)
        {
            rx_minrto_ = KCP_RTO_NDL; //
        }
        else
        {
            rx_minrto_ = KCP_RTO_MIN; //
        }
    }

    if (interval >= 0)
    {
        if (interval > 5000)
        {
            interval = 5000;
        }
        else if (interval < 10)
        {
            interval = 10;
        }
        interval_ = interval;
    }
    if (resend >= 0)
    {
        fastresend_ = resend;
    }
    nocwnd_ = nocwnd;
}

void Kcpp::set_wndsize(int sndwnd, int rcvwnd)
{
    if (sndwnd > 0)
    {
        snd_wnd_ = sndwnd;
    }

    if (rcvwnd > 0)
    {
        rcv_wnd_ = std::max(KCP_WND_RCV, static_cast<uint32_t>(rcvwnd));
    }
}

// size of data that has not been send
int Kcpp::wait_send_size()
{
    return send_buf_.size() + send_queue_.size();
}

// send data
// push data into send queue
int Kcpp::send(const char *data, int len)
{
    assert(mss_ > 0); // mss must be set
    assert(len >= 0); // len must be positive

    // append to previous segment in streaming mode (if possible)
    if (stream_ != false)
    {
        if (!send_queue_.empty())
        {
            auto &old = send_queue_.front();

            if (old->header_.len < mss_)
            {
                int capacity = mss_ - old->header_.len;
                int extend = std::min(len, capacity);

                kcpSegPtr seg = std::make_unique<kcpSeg>(old->header_.len + extend);

                memcpy(seg->data, old->data, old->header_.len);
                old->copy_data2buf(seg->data);

                if (data)
                {
                    memcpy(seg->data + old->header_.len, data, extend);
                    data += extend;
                }

                seg->header_.len = old->header_.len + extend;
                seg->header_.frg = 0;
                len -= extend;
                send_queue_.push_back(std::move(seg));
                send_queue_.pop_front();
            }
        }

        if (len <= 0)
        {
            return 0;
        }
    }

    int count = 0;
    if (len <= static_cast<int>(mss_))
        count = 1;
    else
        count = (len + mss_ - 1) / mss_;

    if (count >= static_cast<int>(KCP_WND_RCV))
        return -2;

    if (count == 0)
        count = 1;

    // fragment ,just set len and frg in header ,other data will be set in mv_queue_to_buf
    for (int i = 0; i < count; i++)
    {
        int size = std::min(len, static_cast<int>(mss_));
        kcpSegPtr seg = std::make_unique<kcpSeg>(size);

        if (data && len > 0)
        {
            seg->set_data(data, size);
        }

        seg->header_.len = size;
        seg->header_.frg = stream_ ? 0 : (count - i - 1);

        send_queue_.push_back(std::move(seg));
        if (data)
        {
            data += size;
        }
        len -= size;
    }

    return 0;
}

// receive data
// mv data from rcv_queue to buffer
int Kcpp::recv(char *buffer, int len)
{
    assert(len >= 0);          // len must be positive
    assert(buffer != nullptr); // buffer must be valid

    bool peek_flag = len < 0 ? true : false;

    int peeksize = peek_size();
    bool recover_flag = false;

    if (rcv_queue_.empty()) // no data
    {
        return -1;
    }
    else if (peeksize < 0) // no data
    {
        return -2;
    }
    else if (peeksize > len) // buffer is not enough
    {
        return -3;
    }

    if (rcv_queue_.size() > rcv_wnd_)
    {
        recover_flag = true;
    }

    len = 0;

    for (auto it = rcv_queue_.begin(); it != rcv_queue_.end();)
    {
        int fragment = (*it)->header_.frg;
        buffer = (*it)->copy_data2buf(buffer);
        len += (*it)->header_.len;

        it = rcv_queue_.erase(it);

        if (fragment == 0)
            break;
    }


    // move available data from rcv_buf -> rcv_queue
    mv_buf_to_queue();

    if (rcv_queue_.size() < rcv_wnd_ && recover_flag)
    {
        // ready to send back IKCP_CMD_WINS in ikcp_flush
        // tell remote my window size
        probe_ |= KCP_ASK_TELL;
    }

    return len;
}

// update state (call it repeatedly, every 10ms-100ms), or you can ask
void Kcpp::update(uint32_t current)
{
    current_ = current;
    if (updated_ == false) // first call
    {
        updated_ = true;
        ts_flush_ = current_ + interval_;
    }
    int slap = static_cast<int>(current_ - ts_flush_); // time diff
    if (slap >= 10000 || slap < -10000)                // time diff is too big
    {
        ts_flush_ = current_;
        slap = 0;
    }
    if (slap >= 0) // time diff is ok
    {
        ts_flush_ += interval_;
        if (current_ - ts_flush_ >= interval_)
        {
            ts_flush_ = current_ + interval_;
        }
        flush();
    }
}

int32_t Kcpp::check(uint32_t current)
{
    uint32_t ts_flush = ts_flush_;
    int32_t tm_flush = std::numeric_limits<int32_t>::max();
    int32_t tm_packet = std::numeric_limits<int32_t>::max();
    uint32_t minimal = 0;

    if (updated_ == false)
    {
        return current;
    }

    if (_itimediff(current, ts_flush) >= 10000 ||
        _itimediff(current, ts_flush) < -10000)
    {
        ts_flush = current;
    }

    if (current >= ts_flush)
    {
        return current;
    }

    tm_flush = _itimediff(ts_flush, current);

    for (auto &seg : send_buf_)
    {
        int diff = _itimediff(seg->header_.resendts, current);
        if (diff <= 0)
        {
            return current;
        }
        if (diff < tm_packet)
            tm_packet = diff;
    }

    minimal = std::min(tm_packet, tm_flush);
    if (minimal >= interval_)
        minimal = interval_;

    return current + minimal;
}

// callback function
int Kcpp::output(const char *data, int size)
{
    if (size == 0)
        return 0;
    return output_(data, size, this, this->user_);
}

void Kcpp::update_probe()
{
    if (rmt_wnd_ == 0)
    {
        if (probe_wait_ == 0)
        {
            probe_wait_ = KCP_PROBE_INIT;
            ts_probe_ = current_ + probe_wait_;
        }
        else
        {
            if (current_ >= ts_probe_)
            {
                if (probe_wait_ < KCP_PROBE_INIT)
                    probe_wait_ = KCP_PROBE_INIT;

                probe_wait_ += probe_wait_ / 2;

                if (probe_wait_ > KCP_PROBE_LIMIT)
                    probe_wait_ = KCP_PROBE_LIMIT;

                ts_probe_ = current_ + probe_wait_;
                probe_ |= KCP_ASK_SEND;
            }
        }
    }
    else
    {
        ts_probe_ = 0;
        probe_wait_ = 0;
    }
}

// flush exist data
void Kcpp::flush()
{

    // 'update' haven't been called.
    if (updated_ == false)
    {
        return;
    }
    // flush acknowledges
    flush_ack();

    // probe window size (if remote window size equals zero)
    update_probe();
    // flush window probing commands
    flush_window_probe();

    // move data from snd_queue to snd_buf
    mv_queue_to_buf();
    // flush data segments
    flush_data();
}

int Kcpp::peek_size()
{
    if (rcv_queue_.empty()) // no data
    {
        return -1;
    }

    auto &seg = rcv_queue_.front();

    if (seg->header_.frg == 0) // only one segment
    {
        return seg->header_.len;
    }

    if (rcv_queue_.size() < seg->header_.frg + 1) // no enough segments
    {
        return -1;
    }

    int length = 0;
    for (auto &seg : rcv_queue_) // calculate length
    {
        length += seg->header_.len;
        if (seg->header_.frg == 0) // last segment
        {
            break;
        }
    }
    return length;
}

void Kcpp::parse_fastack(uint32_t sn, uint32_t ts)
{

    if (sn < snd_una_ || sn >= snd_nxt_) // invalid sn
        return;

    for (auto &seg : send_buf_)
    {
        if (sn != seg->header_.sn)
        {
#ifndef KCP_FASTACK_CONSERVE
            seg->header_.fastack++;
#else
            if (ts >= seg->ts)
                seg->header_.fastack++;
#endif
        }
        else if (sn < seg->header_.sn)
        {
            break;
        }
    }
}

void Kcpp::update_ack(int rtt)
{
    int32_t rto = 0;
    if (rx_srtt_ == 0) // first time
    {
        rx_srtt_ = rtt;
        rx_rttval_ = rtt / 2;
    }
    else
    {
        long delta = rtt - rx_srtt_;
        if (delta < 0)
            delta = -delta;

        rx_rttval_ = (3 * rx_rttval_ + delta) / 4;
        rx_srtt_ = (7 * rx_srtt_ + rtt) / 8;
        if (rx_srtt_ < 1)
            rx_srtt_ = 1;
    }

    rto = rx_srtt_ + std::max(interval_, static_cast<uint32_t>(4 * rx_rttval_));

    rx_rto_ = std::min(static_cast<uint32_t>(std::max(rx_minrto_, rto)), KCP_RTO_MAX);
}

// remove the first snd_buf segment which sn equals to 'sn'
void Kcpp::remove_ack(uint32_t sn)
{

    if (sn < snd_una_ || sn >= snd_nxt_) // out of range
    {
        return;
    }

    for (auto it = send_buf_.begin(); it != send_buf_.end(); ++it)
    {
        auto &seg = *it;
        if (sn == seg->header_.sn) // find the segment and remove it
        {
            send_buf_.erase(it);
            break;
        }
        if (sn < seg->header_.sn) // the  segment of sn is not exist
        {
            break;
        }
    }
}

// check if the data is repeat, if repeat throw it away , else put it into rcv_buf
void Kcpp::check_data_repeat(kcpSegPtr newseg)
{
    uint32_t sn = newseg->header_.sn;
    bool repeat_flag = false;

    if (sn >= rcv_nxt_ + rcv_wnd_ || sn < rcv_nxt_) // out of window
    {
        return;
    }

    for (auto &seg : rcv_buf_)
    {
        if (sn == seg->header_.sn) // repeat
        {
            repeat_flag = true;
            break;
        }
        if (sn < seg->header_.sn) // out of order
        {
            break;
        }
    }

    if (repeat_flag == false)
    {
        rcv_buf_.push_back(std::move(newseg));
    }

    // move available data from rcv_buf to rcv_queue
    mv_buf_to_queue();
}

// remove the segments before una from snd_buf
void Kcpp::remove_before_una(uint32_t una)
{
    auto it = send_buf_.begin();
    while (it != send_buf_.end())
    {
        if (una > (*it)->header_.sn)
        {
            it = send_buf_.erase(it);
        }
        else
        {
            break;
        }
    }
}

// if snd_buf is empty, reset snd_una and snd_nxt
void Kcpp::shrink_buf()
{
    snd_una_ = send_buf_.empty() ? snd_nxt_ : send_buf_.front()->header_.sn;
}

// get data from UDP
int Kcpp::input(const char *data, uint32_t size)
{
    // if data is empty OR size is less than KCP_OVERHEAD,  data is invalid
    if (data == nullptr || size < KCP_OVERHEAD)
        return -1;

    uint32_t prev_una = snd_una_;
    uint32_t maxack = 0;
    uint32_t latest_ts = 0;
    bool flag = false;

    while (true)
    {
        kcpSeg segment;

        if (size < static_cast<int>(KCP_OVERHEAD))
            break;

        segment.parse_header(data);
        data += KCP_OVERHEAD;
        size -= KCP_OVERHEAD;

        if (segment.header_.conv != conv_) // conv is not match
        {
            return -1;
        }
        else if (size < static_cast<int>(segment.header_.len)) // data is imcomplete
        {
            return -1;
        }

        if (size < segment.header_.len)
        {
            return -2;
        }
        if (static_cast<long>(segment.header_.len) < 0 || static_cast<long>(size) < static_cast<long>(segment.header_.len))
        {
            return -2;
        }

        if (segment.header_.cmd != KCP_CMD_PUSH && segment.header_.cmd != KCP_CMD_ACK &&
            segment.header_.cmd != KCP_CMD_WASK && segment.header_.cmd != KCP_CMD_WINS)
            return -3;

        rmt_wnd_ = segment.header_.wnd;
        remove_before_una(segment.header_.una);
        shrink_buf();

        if (segment.header_.cmd == KCP_CMD_ACK) // ACK
        {
            if (current_ >= segment.header_.ts)
            {
                update_ack(static_cast<int>(current_ - segment.header_.ts));
            }
            remove_ack(segment.header_.sn);
            shrink_buf();
            if (!flag)
            {
                flag = true;
                maxack = segment.header_.sn;
                latest_ts = segment.header_.ts;
            }
            else
            {
                if (segment.header_.sn > maxack)
                {
                    maxack = segment.header_.sn;
                    latest_ts = segment.header_.ts;
                }
            }
            // log here
        }
        else if (segment.header_.cmd == KCP_CMD_PUSH) // PUSH
        {
            // log here
            if (_itimediff(segment.header_.sn, rcv_nxt_ + rcv_wnd_) < 0)
            {
                acklist_.push_back({segment.header_.sn, segment.header_.ts});

                if (segment.header_.sn >= rcv_nxt_)
                {
                    kcpSegPtr seg = std::make_unique<kcpSeg>(segment);

                    if (segment.header_.len > 0)
                    {
                        memcpy(seg->data, data, segment.header_.len);
                    }
                    check_data_repeat(std::move(seg));
                }
            }
        }
        else if (segment.header_.cmd == KCP_CMD_WASK)
        {
            // ready to send back KCP_CMD_WINS in KCP_flush
            // tell remote my window size
            probe_ |= KCP_ASK_SEND;
            // log here
        }
        else if (segment.header_.cmd == KCP_CMD_WINS)
        {
            // do nothing
        }

        data += segment.header_.len;
        size -= segment.header_.len;
    }

    if (flag)
    {
        parse_fastack(maxack, latest_ts);
    }

    if (snd_una_ > prev_una) //
    {
        if (cwnd_ < rmt_wnd_)
        {
            uint32_t mss = mss_;
            if (cwnd_ < ssthresh_)
            {
                cwnd_++;
                incr_ += mss;
            }
            else
            {
                if (incr_ < mss)
                    incr_ = mss;
                incr_ += (mss * mss) / incr_ + (mss / 16);
                if ((cwnd_ + 1) * mss <= incr_)
                {
                    cwnd_++;
                }
            }
            if (cwnd_ > rmt_wnd_)
            {
                cwnd_ = rmt_wnd_;
                incr_ = rmt_wnd_ * mss_;
            }
        }
    }

    return 0;
}

//
int Kcpp::wnd_unused()
{
    if (rcv_queue_.size() < rcv_wnd_)
        return static_cast<int>(rcv_wnd_ - rcv_queue_.size());
    return 0;
}

// move data from rcv_buf to rcv_queue
void Kcpp::mv_buf_to_queue()
{
    while (!rcv_buf_.empty())
    {
        auto &seg = rcv_buf_.front();
        if (seg->header_.sn == rcv_nxt_ && rcv_queue_.size() < rcv_wnd_)
        {
            rcv_queue_.push_back(std::move(seg));
            rcv_buf_.pop_front();
            rcv_nxt_++;
        }
        else
        {
            break;
        }
    }
}

// move data from snd_queue to snd_buf
void Kcpp::mv_queue_to_buf()
{
    uint32_t cwnd = std::min(snd_wnd_, rmt_wnd_); // cwnd is the minimum of snd_wnd_ and rmt_wnd_
    if (nocwnd_ == false)                         // if nocwnd is false, cwnd is the minimum of cwnd and snd_buf_.size()
        cwnd = std::min(cwnd_, cwnd);

    // if snd_buf_.size() is less than cwnd, we can send more data
    while (snd_nxt_ < snd_una_ + cwnd && !send_queue_.empty())
    {
        auto &newseg = send_queue_.front();
        newseg->header_.conv = conv_;
        newseg->header_.cmd = KCP_CMD_PUSH;
        newseg->header_.wnd = wnd_unused();
        newseg->header_.ts = current_;
        newseg->header_.sn = snd_nxt_++;
        newseg->header_.una = rcv_nxt_;
        newseg->header_.resendts = current_;
        newseg->header_.rto = rx_rto_;

        send_buf_.push_back(std::move(newseg)); // move the data from snd_queue_ to snd_buf_
        send_queue_.pop_front();
    }
}

// flush all acks
void Kcpp::flush_ack()
{
    char *ptr = buffer_;
    kcpSeg seg;

    seg.header_.conv = conv_;
    seg.header_.cmd = KCP_CMD_ACK;
    seg.header_.wnd = wnd_unused();
    seg.header_.una = rcv_nxt_;

    // flush acknowledges
    for (auto &ack : acklist_)
    {
        ptr = try_output(ptr);
        seg.header_.sn = ack[0];
        seg.header_.ts = ack[1];
        ptr = seg.copy_header2buf(ptr);
    }
    acklist_.clear();
}

// if buffer is more than mtu, send it
char *Kcpp::try_output(char *ptr)
{
    int size = static_cast<int>(ptr - buffer_);
    if (size + static_cast<int>(KCP_OVERHEAD) > static_cast<int>(mtu_))
    {
        output(buffer_, size);
        ptr = buffer_;
    }
    return ptr;
}

void Kcpp::flush_window_probe()
{
    char *ptr = buffer_;
    kcpSeg seg;

    seg.header_.conv = conv_;
    seg.header_.cmd = KCP_CMD_ACK;
    seg.header_.wnd = wnd_unused();
    seg.header_.una = rcv_nxt_;

    // flush window probing commands
    if (probe_ & KCP_ASK_SEND)
    {
        seg.header_.cmd = KCP_CMD_WASK;
        ptr = try_output(ptr);
        ptr = seg.copy_header2buf(ptr);
    }

    // flush window probing commands
    if (probe_ & KCP_ASK_TELL)
    {
        seg.header_.cmd = KCP_CMD_WINS;
        ptr = try_output(ptr);
        ptr = seg.copy_header2buf(ptr);
    }

    probe_ = 0;
}

// flush data
void Kcpp::flush_data()
{

    bool change = false, lost = false;

    char *ptr = buffer_;

    kcpSeg seg;
    seg.header_.conv = conv_;
    seg.header_.cmd = KCP_CMD_ACK;
    seg.header_.wnd = wnd_unused();
    seg.header_.una = rcv_nxt_;

    uint32_t resent = (fastresend_ > 0) ? static_cast<uint32_t>(fastresend_) : std::numeric_limits<uint32_t>::max();
    uint32_t rtomin = (nodelay_ == 0) ? (rx_rto_ >> 3) : 0;

    for (auto &segment : send_buf_)
    {
        bool needsend = false;
        if (segment->header_.xmit == 0) // first time to send
        {
            needsend = true;
            segment->header_.xmit++;
            segment->header_.rto = rx_rto_;
            segment->header_.resendts = current_ + segment->header_.rto + rtomin; // resend time
        }
        else if (current_ >= segment->header_.resendts) // resend
        {
            needsend = true;
            segment->header_.xmit++;
            xmit_++;
            if (nodelay_ == 0)
            {
                segment->header_.rto += std::max(segment->header_.rto, static_cast<uint32_t>(rx_rto_));
            }
            else
            {
                segment->header_.rto += rx_rto_;
            }
            segment->header_.resendts = current_ + segment->header_.rto;
            lost = true; // lost
        }
        else if (segment->header_.fastack >= resent) // fast resend
        {
            needsend = true;
            segment->header_.xmit++;
            segment->header_.fastack = 0;
            segment->header_.resendts = current_ + segment->header_.rto;
            change = true;
        }

        if (needsend)
        {
            segment->header_.ts = current_;
            segment->header_.wnd = seg.header_.wnd;
            segment->header_.una = rcv_nxt_;

            ptr = try_output(ptr);

            ptr = segment->copy_header2buf(ptr);
            ptr = segment->copy_data2buf(ptr);

            if (segment->header_.xmit >= dead_link_)
            {
                state_ = false;
            }
        }
    }
    int size = static_cast<int>(ptr - buffer_);
    if (size > 0)
    {
        output(buffer_, size);
    }

    if (change)
    {
        uint32_t inflight = snd_nxt_ - snd_una_;
        ssthresh_ = std::max(inflight / 2, KCP_THRESH_MIN);
        cwnd_ = ssthresh_ + resent;
        incr_ = cwnd_ * mss_;
    }

    if (lost)
    {
        ssthresh_ = std::max(cwnd_ / 2, KCP_THRESH_MIN);
        cwnd_ = 1;
        incr_ = mss_;
    }
    if (cwnd_ < 1)
    {
        cwnd_ = 1;
        incr_ = mss_;
    }
}