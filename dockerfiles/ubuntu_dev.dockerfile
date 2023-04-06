
FROM ubuntu:20.04
ENV DEBIAN_FRONTEND noninteractive
RUN apt-get update && \
    apt-get install -y cmake clang git && \
    apt-get install -y libssl-dev libsodium-dev && \
    apt-get install -y uuid-dev && \
    apt-get install -y libgnutls28-dev && \
    apt-get install -y software-properties-common && \
    add-apt-repository ppa:git-core/ppa && apt update && apt install -y git
RUN touch //.gitconfig && chmod 777 //.gitconfig

