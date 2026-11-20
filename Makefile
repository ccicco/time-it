.RECIPEPREFIX := >
CXX      := clang++
CXXFLAGS := -std=c++20 -Wall -Wextra -O0 -g -Iinclude
LDLIBS   := -lncurses
BUILD    := build
BIN      := $(BUILD)/timeit
SRCS     := src/main.cpp src/connection/net.cpp src/input/input.cpp src/display/view.cpp src/display/paint.cpp
OBJS     := $(SRCS:src/%.cpp=$(BUILD)/%.o)

all: $(BIN)

$(BIN): $(OBJS)
>$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDLIBS)

$(BUILD)/%.o: src/%.cpp
>@mkdir -p $(dir $@)
>$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
>rm -rf $(BUILD)
