FROM ubuntu:18.04

RUN apt-get update
RUN apt-get install -y cmake build-essential autoconf git libtool python3-setuptools libboost-all-dev
RUN apt-get install -y golang
RUN apt-get install -y clang-9 libclang-9-dev llvm-9-dev curl gperf libgmp-dev
RUN apt-get install -y cmake bison flex libboost-all-dev python perl minisat

#protobuf
RUN git clone https://github.com/protocolbuffers/protobuf.git /protobuf  && \
    cd /protobuf && \
    git submodule update --init --recursive && \
    ./autogen.sh && \
    ./configure  && \
    make -j && \
    make install && \
    ldconfig



WORKDIR /src
#gperf
RUN cd /src && git clone https://github.com/gperftools/gperftools.git && cd gperftools && \
     git checkout gperftools-2.9.1 && ./autogen.sh && ./configure && make -j && make install && ldconfig

COPY . /src/jigsaw

RUN cd /src/jigsaw/ && mkdir build && cd build && cmake .. && make rgd -j


