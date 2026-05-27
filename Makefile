CXX       = g++
CXXFLAGS  = -std=c++11 -Wall -Wextra -O2
LDFLAGS   =
SRCDIR    = src
OBJDIR    = obj
TARGET    = use-linux-perf

SRCS      = $(SRCDIR)/main.cpp $(SRCDIR)/executor.cpp $(SRCDIR)/utils.cpp \
            $(SRCDIR)/collectors.cpp $(SRCDIR)/reporters.cpp
OBJS      = $(patsubst $(SRCDIR)/%.cpp, $(OBJDIR)/%.o, $(SRCS))

.PHONY: all clean

all: $(OBJDIR) $(TARGET)

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJDIR)/%.o: $(SRCDIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -rf $(OBJDIR) $(TARGET)
