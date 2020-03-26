#include <iostream>
#include <sstream>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <list>

#include <ffmpeg.hpp>

struct queue {
	queue(size_t max_capacity = 90) : max(max_capacity), closed(false), t0(AV_NOPTS_VALUE) {}

	void enqueue(av::frame &f) {
		std::lock_guard<std::mutex> l(m);
#if 0
		std::cerr << (void*)this << std::dec
			  << ": queue size: " << filled.size()
			  << " enqueue frame: " << f.f->pts
			  << std::endl;
#endif
		filled.push_back(f);

		if (filled.size() > max)
			filled.pop_front();

		cv.notify_one();
	}

	av::frame acquire() {
		av::frame f;
		std::unique_lock<std::mutex> l(m);

		while (filled.empty() && !closed)
			cv.wait(l);

		if (!filled.empty()) {
			f = filled.front();
			filled.pop_front();
		}

		return f;
	}

	bool acquire(av::frame &f, int64_t pts) {
		std::unique_lock<std::mutex> l(m);

		if (filled.empty())
			return false;

		if (pts < filled.front().f->pts + t0) {
			/*
			 * we are reading faster than the other stream
			 * the only choice is to let the other keeps
			 * reading
			 */
			return false;
		}

		while ((pts > filled.back().f->pts + t0) && !closed) {
			/*
			 * the other thread is in advance just wait a
			 * little bit to receive some new frames
			 */
			cv.wait(l);
		}

		if (closed)
			return false;

		/*
		 * we should be able to find the corresponding frame
		 * now
		 * please rewrite this part it sucks!
		 */
		while (!filled.empty() && pts >= filled.front().f->pts + t0) {
			f = filled.front();
			filled.pop_front();
		}

		return true;
	}

	void close(bool clear = false) {
		std::lock_guard<std::mutex> l(m);

		closed = true;
		if (clear)
			filled.clear();

		cv.notify_all();
	}

	bool is_closed() {
		std::lock_guard<std::mutex> l(m);

		return closed && filled.empty();
	}

	size_t size() {
		std::lock_guard<std::mutex> l(m);

		return filled.size();
	}

	std::mutex m;
	std::condition_variable cv;
	std::list<av::frame> filled;
	size_t max;
	bool closed;
	int64_t t0;
};

static void read_input(av::input &in, queue &q)
{
	av::hw_device accel;
#if 0
	accel = av::hw_device("cuda");
	if (!accel)
		accel = av::hw_device("vaapi");
#endif
	av::decoder dec = in.get(accel, 0);
	if (!dec) {
		std::cerr << "Unable to get a decoder" << std::endl;
		return;
	}

	av::packet p;
	av::frame f;

	while (in >> p) {
		if (p.stream_index() != 0)
			continue;

		dec << p;

		while (dec >> f)
			q.enqueue(f);
	}

	// flush
	dec.flush();
	while (dec >> f)
		q.enqueue(f);
}

static void read_video(const std::string &url, queue &q)
{
	av::input in;
	bool is_rtsp;

	is_rtsp = (url.compare(0,7, "rtsp://") == 0);

	if (in.open(url, is_rtsp ? "rtsp_transport=tcp" : "")) {
		int64_t t0;

		if (is_rtsp) {
			// get t0 from start_real_time
			av::packet p;

			while (in >> p) {
				int64_t realtime = in.start_time_realtime();

				if (realtime != AV_NOPTS_VALUE) {
					t0 = realtime;
					break;
				}
			}
		} else {
			// get t0 from metadata
			std::stringstream sstr(in.program_metadata(0));
			std::string tmp;

			while (getline(sstr, tmp, ':')) {
				auto equal = tmp.find('=');

				if (equal == std::string::npos)
					continue;

				if (tmp.compare(0, equal, "service_name") == 0) {
					tmp.erase(0, equal + 1);

					t0 = std::stoll(tmp);
					break;
				}
			}
		}

		std::cerr << "t0: " << t0 << std::endl;

		AVRational time_base = in.time_base(0);
		q.t0 = av_rescale(t0, time_base.den, time_base.num * 1000000);

		read_input(in, q);
	} else
		std::cerr << "Unable to open '" << url << "'" << std::endl;

	q.close();
}

int main(int argc, char *argv[])
{
	if (argc < 3) {
		std::cerr << "Usage: " << argv[0]
			  << " <url> <url>" << std::endl;
		return -1;
	}

	queue q1, q2;
	std::thread t1,t2;

	t1 = std::thread(read_video, argv[1], std::ref(q1));
	t2 = std::thread(read_video, argv[2], std::ref(q2));

	while (!q1.is_closed() && !q2.is_closed()) {
		// start reading frame here
		av::frame f1, f2;

		std::cerr << "\r\033[2L";
		std::cerr << "q1: " << q1.size() << " q2: " << q2.size();

		f1 = q1.acquire();

		if (q2.acquire(f2, f1.f->pts + q1.t0)) {
			// do something with f1 and f2
		} else {
			std::cerr << std::endl << "can't find a second frame" << std::endl;
		}

	}

	t1.join();
	t2.join();
	return 0;
}
