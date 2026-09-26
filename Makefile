FLAGS_SL = -g -O3 -Wall -fno-common -Isqueezelite -Wno-error=incompatible-pointer-types -fpermissive

OWN_UPNP_SOURCES = upnp/xml.cpp upnp/http.cpp upnp/soap.cpp upnp/discovery.cpp upnp/own_speaker_control.cpp
UPNP_OBJS = $(OWN_UPNP_SOURCES:.cpp=.o) upnp/encoded_buffer.o upnp/noson_stream_server.o upnp/noson_speaker_control.o

OBJS = $(UPNP_OBJS) sonos-lms.o sbstreamer.o sbencoder.o sonos-status.o sonos-position.o

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

all: sonos-lms

noson/noson/libnoson.a: noson/CMakeLists.txt noson/noson/CMakeLists.txt
	cmake -D CMAKE_POLICY_VERSION_MINIMUM=3.5 -D CMAKE_BUILD_TYPE=Release -S noson -B noson
	make -C noson

%.o: %.cpp noson/noson/libnoson.a
	g++ -g -O3 -Wall -Inoson/noson/src -Inoson/noson/public/noson -c -o $@ $<

%.o: %.c
	gcc $(FLAGS_SL) -c -o $@ $<

squeezelite.o: squeezelite.cpp
	g++ $(FLAGS_SL) -c -o $@ $<

sonos-lms: $(OBJS) $(OBJS_SL) noson/noson/libnoson.a
	g++ -g -o $@ $^ \
		-Lnoson/noson -lnoson \
		-lFLAC++ -lFLAC -lcrypto -lssl -lz \
		-lpthread -lm -lrt -ldl -lasound

clean:
	rm -f *.o upnp/*.o squeezelite/*.o sonos-lms position-test encoder-test resume-state-test streamer-test upnp-test own-control-test noson-golden

slimproto_sonos.o: slimproto_sonos.c squeezelite/slimproto.c squeezelite/squeezelite.h

.PHONY: test install
install: sonos-lms
	scripts/install-devices.sh
encoder-test: upnp/encoded_buffer.cpp upnp/encoded_buffer.h tests/encoder_test.cpp sbencoder.cpp sbencoder.h noson/noson/libnoson.a
	g++ -g -O2 -Wall -I. -Inoson/noson/src -Inoson/noson/public/noson -DSBENCODER_TEST -o $@ tests/encoder_test.cpp sbencoder.cpp upnp/encoded_buffer.cpp noson/noson/libnoson.a -lFLAC++ -lFLAC -lcrypto -lssl -lz -lpthread

test: sonos-lms position-test encoder-test resume-state-test streamer-test upnp-test own-control-test noson-golden
	./upnp-test
	python3 tests/list_rooms_test.py
	python3 tests/installer_test.py
	python3 tests/upnp_mock_test.py
	python3 tests/own_display_test.py
	./position-test
	python3 tests/send_error_test.py
	./streamer-test session
	./streamer-test position
	./streamer-test shutdown
	python3 tests/output_shutdown_test.py
	./encoder-test
	./resume-state-test
	SONOS_LMS_PAUSE=pause ./streamer-test
	env -u SONOS_LMS_PAUSE ./streamer-test stop
	python3 tests/device_resume_test.py
	python3 tests/lms_discovery_test.py
	python3 tests/device_test_script_test.py
	python3 tests/pause_mode_test.py

sbstreamer.o sbencoder.o: sbencoder.h

sonos-lms.o: resume_state.h stop_debounce.h
resume-state-test: tests/resume_state_test.cpp resume_state.h stop_debounce.h
	g++ -g -O2 -Wall -I. -o $@ tests/resume_state_test.cpp

streamer-test: upnp/encoded_buffer.cpp upnp/encoded_buffer.h upnp/noson_stream_server.cpp pause_mode.h tests/streamer_test.cpp sbstreamer.cpp sbstreamer.h sbencoder.cpp sbencoder.h resume_state.h noson/noson/libnoson.a
	g++ -g -O2 -Wall -I. -Inoson/noson/src -Inoson/noson/public/noson -o $@ tests/streamer_test.cpp sbstreamer.cpp sbencoder.cpp sonos-position.cpp upnp/noson_stream_server.cpp upnp/encoded_buffer.cpp noson/noson/libnoson.a -lFLAC++ -lFLAC -lcrypto -lssl -lz -lpthread

sonos-lms.o streamer-test: pause_mode.h

position-test: tests/position_test.cpp position_state.h
	g++ -g -O2 -Wall -I. -o $@ tests/position_test.cpp
sonos-position.o: position_state.h sonos-position.h
output_sonos.o sonos-lms.o sbstreamer.o streamer-test: sonos-position.h
streamer-test: sonos-position.cpp position_state.h

sonos-lms.o: transport_intent.h retry_budget.h

sonos-lms.o sbstreamer.o streamer-test: stream_session.h

$(OBJS) streamer-test: upnp/speaker_control.h upnp/stream_server.h upnp/noson_stream_server.h upnp/noson_speaker_control.h

$(UPNP_OBJS) sonos-lms.o: upnp/xml.h upnp/http.h upnp/soap.h upnp/discovery.h upnp/own_speaker_control.h upnp/backend.h

upnp-test: $(wildcard upnp/*.h) tests/upnp_test.cpp upnp/xml.cpp upnp/soap.cpp upnp/http.cpp upnp/discovery.cpp
	g++ -g -O2 -Wall -Wextra -I. -o $@ $(filter %.cpp,$^)

own-control-test: $(wildcard upnp/*.h) tests/own_control_fixture.cpp $(OWN_UPNP_SOURCES)
	g++ -g -O2 -Wall -Wextra -I. -o $@ $(filter %.cpp,$^) -lpthread

noson-golden: tests/noson_golden.cpp noson/noson/libnoson.a
	g++ -g -O2 -Wall -Inoson/noson/src -Inoson/noson/public/noson -o $@ $^ -lcrypto -lssl -lz -lpthread


upnp/encoded_buffer.o sbencoder.o: upnp/encoded_buffer.h

sonos-lms.o: upnp/list_rooms.h

sbstreamer.o streamer-test: stream_close_log.h
