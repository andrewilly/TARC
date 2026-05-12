FROM gcr.io/oss-fuzz-base/base-builder:latest

RUN apt-get update && apt-get install -y \
    libzstd-dev \
    liblz4-dev \
    liblzma-dev \
    libbrotli-dev

COPY . $SRC/tarc
WORKDIR $SRC/tarc

COPY fuzz/ossfuzz/build.sh $SRC/build.sh
