TOOL64 := /opt/homebrew/bin/x86_64-w64-mingw32
TOOL32 := /opt/homebrew/bin/i686-w64-mingw32
FRONT := src/front
TEST := tests/front
BUILD := build
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -MMD -MP
CPPFLAGS := -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX
GUI := -static -mwindows -municode
RPC := -lrpcrt4 -ladvapi32 -luser32
BRIDGE_LIBS := -lshell32 -lsecur32

ifeq ($(SPEAKER),log)
SPEAKER_SOURCES := $(FRONT)/log_speaker.cpp
else ifeq ($(SPEAKER),sapi)
ifeq ($(wildcard src/voice/LIBS.txt),)
$(error The SAPI speaker requires src/voice/LIBS.txt)
endif
SPEAKER_SOURCES := $(wildcard src/voice/*.cpp)
SPEAKER_LIBS_FILE := src/voice/LIBS.txt
SPEAKER_LIBS := $(shell cat $(SPEAKER_LIBS_FILE))
else
$(error SPEAKER must be log or sapi)
endif

SERVER_SOURCES := $(filter-out $(FRONT)/log_speaker.cpp,$(wildcard $(FRONT)/*.cpp)) $(SPEAKER_SOURCES)
SERVER_OBJECTS := $(patsubst src/%.cpp,$(BUILD)/x64/%.o,$(SERVER_SOURCES)) $(BUILD)/x64/controller_s.o $(BUILD)/x64/controller_c.o

.PHONY: all tests FORCE
all: $(BUILD)/VoiceBridge.exe

$(BUILD)/speaker-mode: FORCE
	@mkdir -p $(BUILD)
	@if [ "$$(cat $@ 2>/dev/null)" != "$(SPEAKER)" ]; then printf '%s\n' '$(SPEAKER)' > $@; fi

$(BUILD)/VoiceBridge.exe: $(SERVER_OBJECTS) $(BUILD)/speaker-mode $(FRONT)/build.mk $(SPEAKER_LIBS_FILE)
	$(TOOL64)-g++ $(GUI) -o $@ $(SERVER_OBJECTS) $(RPC) $(BRIDGE_LIBS) $(SPEAKER_LIBS)

$(BUILD)/x64/front/ping.o: CXXFLAGS += -fno-exceptions -fno-reorder-blocks

$(BUILD)/x64/%.o: src/%.cpp $(BUILD)/x64/controller.h $(FRONT)/build.mk
	@mkdir -p $(@D)
	$(TOOL64)-g++ $(CPPFLAGS) $(CXXFLAGS) -I$(BUILD)/x64 -c $< -o $@

define controller
$(BUILD)/$(1)/controller.h: $(FRONT)/controller.idl $(FRONT)/build.mk
	@mkdir -p $$(@D)
	$($(2))-widl -m$(3) -Oif --prefix-client=client_ -h -o $$@ $$<

$(BUILD)/$(1)/controller_c.c: $(FRONT)/controller.idl $(FRONT)/build.mk
	@mkdir -p $$(@D)
	$($(2))-widl -m$(3) -Oif --prefix-client=client_ -c -o $$@ $$<

$(BUILD)/$(1)/controller_s.c: $(FRONT)/controller.idl $(FRONT)/build.mk
	@mkdir -p $$(@D)
	$($(2))-widl -m$(3) -Oif -s -o $$@ $$<

$(BUILD)/$(1)/controller_%.o: $(BUILD)/$(1)/controller_%.c $(BUILD)/$(1)/controller.h
	$($(2))-gcc $(CPPFLAGS) -O2 -c $$< -o $$@
endef
$(eval $(call controller,x64,TOOL64,64))
$(eval $(call controller,x86,TOOL32,32))

$(BUILD)/stock.exe: $(TEST)/stock.cpp $(TEST)/test.h $(FRONT)/build.mk
	$(TOOL64)-g++ $(CPPFLAGS) $(filter-out -MMD -MP,$(CXXFLAGS)) $(GUI) -o $@ $<

PRISM := ../d4-assist-fork/templates/ue4ss-prism-bridge/libs/prism/include
$(BUILD)/prism-client.exe: $(TEST)/prism.cpp $(TEST)/test.h $(PRISM)/prism.h $(FRONT)/build.mk
	$(TOOL64)-g++ $(CPPFLAGS) $(filter-out -MMD -MP,$(CXXFLAGS)) $(GUI) -I$(PRISM) -o $@ $<

tests: all $(BUILD)/stock.exe $(BUILD)/prism-client.exe

-include $(SERVER_OBJECTS:.o=.d)

TEST_FLAGS := $(CPPFLAGS) $(filter-out -MMD -MP,$(CXXFLAGS)) $(GUI)
TEST_RPC := $(FRONT)/rpc_alloc.cpp $(FRONT)/endpoint.cpp
CLIENT_DEPS := $(TEST)/client.cpp $(TEST)/test.h $(FRONT)/controller_rpc.h $(TEST_RPC) $(FRONT)/build.mk

$(BUILD)/client64.exe: $(CLIENT_DEPS) $(BUILD)/x64/controller_c.o
	$(TOOL64)-g++ $(TEST_FLAGS) -iquote $(FRONT) -I$(BUILD)/x64 -o $@ $< $(TEST_RPC) $(BUILD)/x64/controller_c.o $(RPC)

$(BUILD)/client32.exe: $(CLIENT_DEPS) $(BUILD)/x86/controller_c.o
	$(TOOL32)-g++ $(TEST_FLAGS) -iquote $(FRONT) -I$(BUILD)/x86 -o $@ $< $(TEST_RPC) $(BUILD)/x86/controller_c.o $(RPC)

$(BUILD)/lifecycle.exe: $(TEST)/lifecycle.cpp $(TEST)/test-process.h $(TEST)/test.h $(FRONT)/controller_rpc.h $(FRONT)/handle.h $(FRONT)/build.mk \
	$(BUILD)/x64/controller_c.o $(BUILD)/x64/controller_s.o \
	$(addprefix $(BUILD)/x64/front/,rpc.o rpc_alloc.o endpoint.o ping.o)
	$(TOOL64)-g++ $(TEST_FLAGS) -iquote $(FRONT) -o $@ $< $(filter %.o,$^) $(RPC)

$(BUILD)/vb-fake-nvda.exe: $(TEST)/fake.cpp $(TEST)/test-process.h $(TEST)/test.h $(FRONT)/handle.h $(FRONT)/build.mk
	$(TOOL64)-g++ $(TEST_FLAGS) -iquote $(FRONT) -o $@ $< -luser32

$(BUILD)/tolk-client.exe: $(TEST)/tolk.cpp $(TEST)/test.h $(FRONT)/build.mk
	$(TOOL64)-g++ $(TEST_FLAGS) -o $@ $<

tests: $(BUILD)/client64.exe $(BUILD)/client32.exe $(BUILD)/lifecycle.exe $(BUILD)/vb-fake-nvda.exe $(BUILD)/tolk-client.exe

ifeq ($(SPEAKER),log)
# Silent fault injection uses the production front and a stalled test speaker.
$(BUILD)/stuck-bridge.exe: $(TEST)/stuck-speaker.cpp $(filter-out $(BUILD)/x64/front/log_speaker.o,$(SERVER_OBJECTS)) $(FRONT)/build.mk
	$(TOOL64)-g++ $(TEST_FLAGS) -o $@ $< $(filter %.o,$^) $(RPC) $(BRIDGE_LIBS)

$(BUILD)/settings-test.exe: $(TEST)/settings-test.cpp $(TEST)/test.h $(BUILD)/x64/front/settings.o $(FRONT)/build.mk
	$(TOOL64)-g++ $(TEST_FLAGS) -o $@ $< $(filter %.o,$^)

# Use the production supervisor and settings with a deterministic logon age.
$(BUILD)/signin-bridge.exe: $(TEST)/test-logon.cpp $(FRONT)/process.h $(filter-out $(BUILD)/x64/front/logon.o,$(SERVER_OBJECTS)) $(FRONT)/build.mk
	$(TOOL64)-g++ $(TEST_FLAGS) -o $@ $< $(filter %.o,$^) $(RPC) $(BRIDGE_LIBS)

tests: $(BUILD)/stuck-bridge.exe $(BUILD)/settings-test.exe $(BUILD)/signin-bridge.exe
endif

# The integration fixture drives the production supervisor and voice engine in
# session zero. Only the test build exposes stalls and packet observations.
RESTART_FRONT_OBJECTS := $(patsubst src/%.cpp,$(BUILD)/restart/%.o,$(filter-out $(FRONT)/log_speaker.cpp,$(wildcard $(FRONT)/*.cpp)))
RESTART_VOICE_OBJECTS := $(patsubst src/%.cpp,$(BUILD)/restart/%.o,$(wildcard src/voice/*.cpp))
RESTART_STUBS := $(addprefix $(BUILD)/x64/,controller_c.o controller_s.o)
RESTART_LIBS := $(shell cat src/voice/LIBS.txt)
RESTART_DRIVER_OBJECT := $(BUILD)/restart/tests/restart-driver.o
RESTART_SPEAKER_OBJECT := $(BUILD)/restart/tests/restart-speaker.o

.PHONY: restart-tests
restart-tests: $(BUILD)/restart-bridge.exe $(BUILD)/restart-driver.exe

$(BUILD)/restart/voice/speaker.o: RESTART_DEFINES := -DCreateServerSpeaker=CreateInstrumentedSpeaker
$(BUILD)/restart/front/main.o: RESTART_DEFINES := -DVB_RESTART_TEST
$(BUILD)/restart/front/ping.o: CXXFLAGS += -fno-exceptions -fno-reorder-blocks

$(BUILD)/restart/%.o: src/%.cpp $(BUILD)/x64/controller.h $(FRONT)/build.mk
	@mkdir -p $(@D)
	$(TOOL64)-g++ $(CPPFLAGS) $(CXXFLAGS) -DVB_VOICE_TEST $(RESTART_DEFINES) -I$(BUILD)/x64 -c $< -o $@

$(BUILD)/restart/tests/%.o: $(TEST)/%.cpp $(BUILD)/x64/controller.h $(FRONT)/build.mk
	@mkdir -p $(@D)
	$(TOOL64)-g++ $(CPPFLAGS) $(CXXFLAGS) -DVB_VOICE_TEST -iquote $(FRONT) -I$(BUILD)/x64 -c $< -o $@

$(BUILD)/restart-bridge.exe: $(RESTART_FRONT_OBJECTS) $(RESTART_VOICE_OBJECTS) $(RESTART_SPEAKER_OBJECT) $(RESTART_STUBS) src/voice/LIBS.txt
	$(TOOL64)-g++ $(GUI) -o $@ $(filter %.o,$^) $(RPC) $(BRIDGE_LIBS) $(RESTART_LIBS)

$(BUILD)/restart-driver.exe: $(RESTART_DRIVER_OBJECT) $(BUILD)/x64/controller_c.o $(addprefix $(BUILD)/restart/front/,rpc_alloc.o endpoint.o ping.o)
	$(TOOL64)-g++ $(GUI) -o $@ $^ $(RPC)

-include $(RESTART_FRONT_OBJECTS:.o=.d) $(RESTART_VOICE_OBJECTS:.o=.d) $(RESTART_DRIVER_OBJECT:.o=.d) $(RESTART_SPEAKER_OBJECT:.o=.d)
