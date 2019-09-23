set -ex && mkdir -p ../../build/release/bin
set -ex && docker create --name sevabit-daemon-container sevabit-daemon-image
set -ex && docker cp sevabit-daemon-container:/usr/local/bin/ ../../build/
set -ex && docker rm sevabit-daemon-container
