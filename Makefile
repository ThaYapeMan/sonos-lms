FLAGS_SL = -g -O3 -Wall -fno-common -Isqueezelite -Wno-error=incompatible-pointer-types -fpermissive

OBJS = sonos-squeezebox.o sbstreamer.o sbencoder.o sonos-status.o sonos-position.o

OBJS_SL = squeezelite.o \
	output_sonos.o \
	slimproto_sonos.o \
	squeezelite/decode.o \
	squeezelite/buffer.o \
	squeezelite/stream.o \
	squeezelite/utils.o \
	squeezelite/output.o \
	squeezelite/output_pack.o \
	squeezelite/flac.o \
	squeezelite/pcm.o \
	squeezelite/vorbis.o \
	squeezelite/faad.o \
	squeezelite/mad.o \
	squeezelite/mpg.o

all: sonos-squeezebox

noson/noson/libnoson.a: noson/CMakeLists.txt noson/noson/CMakeLists.txt
	cmake -D CMAKE_POLICY_VERSION_MINIMUM=3.5 -D CMAKE_BUILD_TYPE=Release -S noson -B noson
	make -C noson

%.o: %.cpp noson/noson/libnoson.a
	g++ -g -O3 -Wall -Inoson/noson/src -Inoson/noson/public/noson -c -o $@ $<

%.o: %.c
	gcc $(FLAGS_SL) -c -o $@ $<

squeezelite.o: squeezelite.cpp
	g++ $(FLAGS_SL) -c -o $@ $<

sonos-squeezebox: $(OBJS) $(OBJS_SL) noson/noson/libnoson.a
	g++ -g -o $@ $^ \
		-Lnoson/noson -lnoson \
		-lFLAC++ -lFLAC -lcrypto -lssl -lz \
		-lpthread -lm -lrt -ldl -lasound

clean:
	rm -f *.o squeezelite/*.o sonos-squeezebox position-test encoder-test resume-state-test streamer-test

slimproto_sonos.o: slimproto_sonos.c squeezelite/slimproto.c squeezelite/squeezelite.h

.PHONY: test install
install:
	scripts/install-devices.sh
encoder-test: tests/encoder_test.cpp sbencoder.cpp sbencoder.h noson/noson/libnoson.a
	g++ -g -O2 -Wall -I. -Inoson/noson/src -Inoson/noson/public/noson -DSBENCODER_TEST -o $@ tests/encoder_test.cpp sbencoder.cpp noson/noson/libnoson.a -lFLAC++ -lFLAC -lcrypto -lssl -lz -lpthread

test: position-test encoder-test resume-state-test streamer-test
	./position-test
	./streamer-test position
	./streamer-test shutdown
	python3 tests/output_shutdown_test.py
	./encoder-test
	./resume-state-test
	SONOS_SQUEEZEBOX_PAUSE=pause ./streamer-test
	env -u SONOS_SQUEEZEBOX_PAUSE ./streamer-test stop
	python3 tests/device_resume_test.py
	python3 tests/lms_discovery_test.py
	python3 tests/pause_mode_test.py

sbstreamer.o sbencoder.o: sbencoder.h

sonos-squeezebox.o: resume_state.h stop_debounce.h
resume-state-test: tests/resume_state_test.cpp resume_state.h stop_debounce.h
	g++ -g -O2 -Wall -I. -o $@ tests/resume_state_test.cpp

streamer-test: pause_mode.h tests/streamer_test.cpp sbstreamer.cpp sbstreamer.h sbencoder.cpp sbencoder.h resume_state.h noson/noson/libnoson.a
	g++ -g -O2 -Wall -I. -Inoson/noson/src -Inoson/noson/public/noson -o $@ tests/streamer_test.cpp sbstreamer.cpp sbencoder.cpp sonos-position.cpp noson/noson/libnoson.a -lFLAC++ -lFLAC -lcrypto -lssl -lz -lpthread

sonos-squeezebox.o streamer-test: pause_mode.h

position-test: tests/position_test.cpp position_state.h
	g++ -g -O2 -Wall -I. -o $@ tests/position_test.cpp
sonos-position.o: position_state.h sonos-position.h
output_sonos.o sonos-squeezebox.o sbstreamer.o streamer-test: sonos-position.h
streamer-test: sonos-position.cpp position_state.h

sonos-squeezebox.o: transport_intent.h retry_budget.h
