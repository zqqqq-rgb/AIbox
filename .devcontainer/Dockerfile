FROM aibox-tests:ubuntu24.04
RUN apt-get -o Acquire::Retries=3 update \
    && DEBIAN_FRONTEND=noninteractive apt-get -o Acquire::Retries=3 install -y --no-install-recommends \
       git gdb ninja-build python3 python3-pip python3-venv nodejs npm curl sqlite3 ffmpeg clang-format \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /workspace
CMD ["sleep", "infinity"]
