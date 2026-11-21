CXXFLAGS= -O3 -fomit-frame-pointer
override CXXFLAGS+= -Wall -fsigned-char -pthread

LIBS=-lglut -lGL
OBJS= \
	demo.o \
	async_pose.o \
	texture.o
GPU_OBJS= \
	gpu-demo.o \
	texture.o

default: all

all: demo gpu-demo

clean:
	-$(RM) $(OBJS) $(GPU_OBJS) demo gpu-demo

demo: $(OBJS)
	$(CXX) $(CXXFLAGS) -o demo $(OBJS) $(LIBS)

gpu-demo: $(GPU_OBJS)
	$(CXX) $(CXXFLAGS) -o gpu-demo $(GPU_OBJS) $(LIBS)

