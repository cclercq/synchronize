#include <iostream>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <list>

#include <ffmpeg.hpp>

struct queue {
	queue(size_t max_capacity = 30) : max(max_capacity), closed(false) {}

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

	std::mutex m;
	std::condition_variable cv;
	std::list<av::frame> filled;
	size_t max;
	bool closed;
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

	if (in.open(url))
		read_input(in, q);
	else
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

		f1 = q1.acquire();

		std::cerr << "f1: " << f1.f->pts << std::endl;
		std::this_thread::sleep_for(std::chrono::milliseconds(500));

		f2 = q2.acquire();

		std::cerr << "f2: " << f2.f->pts << std::endl;
		std::this_thread::sleep_for(std::chrono::milliseconds(500));
	}

	t1.join();
	t2.join();
	return 0;
}
