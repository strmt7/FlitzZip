FROM gcr.io/oss-fuzz-base/base-builder@sha256:18973cd0446a865235843ccc4f6c561d99c27da9a2d32cd0ac55960a31e32975

COPY . $SRC/superzip
WORKDIR $SRC/superzip
COPY .clusterfuzzlite/build.sh $SRC/build.sh
