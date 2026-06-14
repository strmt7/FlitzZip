FROM gcr.io/oss-fuzz-base/base-builder@sha256:18973cd0446a865235843ccc4f6c561d99c27da9a2d32cd0ac55960a31e32975

COPY . $SRC/superzip
WORKDIR $SRC/superzip
COPY .clusterfuzzlite/build.sh $SRC/build.sh
RUN chmod -R a+rX "$SRC/superzip" && chmod a+rx "$SRC/build.sh"

ENV HOME=/tmp
HEALTHCHECK --interval=5m --timeout=10s --start-period=30s --retries=1 CMD [ -r /src/superzip/.clusterfuzzlite/project.yaml ] && [ -x /src/build.sh ] || exit 1
USER nobody:nogroup
