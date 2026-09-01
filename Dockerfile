FROM ubuntu:18.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && \
    apt-get install -y --no-install-recommends \
      proot \
      ca-certificates \
      curl \
      bash \
      tzdata && \
    rm -rf /var/lib/apt/lists/*

CMD ["/bin/bash", "-l"]
