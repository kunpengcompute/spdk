server_spdk_dir=$1
server_spdk_tgt_dir=$server_spdk_dir/build/bin/spdk_tgt
client_ip=$2
client_perf_dir=$3

nohup $server_spdk_tgt_dir -m 0xff &

cd $server_spdk_dir
./scripts/rpc.py nvmf_create_transport -t RDMA -q 1024 -m 127 -c 4096 -i 131072 -u 131072 -a 128 -n 320 -b 0
./scripts/rpc.py bdev_nvme_attach_controller -b Nvme0 -t PCIe -a 0000:17:00.0
./scripts/rpc.py bdev_nvme_attach_controller -b Nvme1 -t PCIe -a 0000:18:00.0
./scripts/rpc.py bdev_nvme_attach_controller -b Nvme2 -t PCIe -a 0000:19:00.0
./scripts/rpc.py bdev_nvme_attach_controller -b Nvme3 -t PCIe -a 0000:1a:00.0
./scripts/rpc.py bdev_nvme_attach_controller -b Nvme4 -t PCIe -a 0000:57:00.0
./scripts/rpc.py bdev_nvme_attach_controller -b Nvme5 -t PCIe -a 0000:58:00.0
./scripts/rpc.py bdev_nvme_attach_controller -b Nvme6 -t PCIe -a 0000:59:00.0
./scripts/rpc.py bdev_nvme_attach_controller -b Nvme7 -t PCIe -a 0000:5a:00.0
./scripts/rpc.py nvmf_create_subsystem nqn.2016-06.io.spdk:cnode1 -a -s SPDK00000000000001 -m 8
./scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Nvme0n1
./scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Nvme1n1
./scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Nvme2n1
./scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Nvme3n1
./scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Nvme4n1
./scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Nvme5n1
./scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Nvme6n1
./scripts/rpc.py nvmf_subsystem_add_ns nqn.2016-06.io.spdk:cnode1 Nvme7n1
./scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t RDMA -a 192.168.65.87 -s 4420
./scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 -t RDMA -a 192.168.75.87 -s 4420

ssh $client_ip "$client_perf_dir -r 'trtype:RDMA adrfam:IPv4 traddr:192.168.65.87 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1 ns:1' -r 'trtype:RDMA adrfam:IPv4 traddr:192.168.65.87 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1 ns:2' -r 'trtype:RDMA adrfam:IPv4 traddr:192.168.65.87 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1 ns:3' -r 'trtype:RDMA adrfam:IPv4 traddr:192.168.65.87 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1 ns:4' -r 'trtype:RDMA adrfam:IPv4 traddr:192.168.75.87 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1 ns:5' -r 'trtype:RDMA adrfam:IPv4 traddr:192.168.75.87 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1 ns:6' -r 'trtype:RDMA adrfam:IPv4 traddr:192.168.75.87 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1 ns:7' -r 'trtype:RDMA adrfam:IPv4 traddr:192.168.75.87 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1 ns:8' -t 60 -w write -o 131072 -q 16 -c 0xff"