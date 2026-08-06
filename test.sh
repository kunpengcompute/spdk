./scripts/rpc.py nvmf_create_transport -t ub 
./scripts/rpc.py bdev_malloc_create 64 512 -b Malloc0
./scripts/rpc.py nvmf_create_subsystem nqn.2022-02.io.spdk:cnode0 -a -s SPDK00000000000001
./scripts/rpc.py nvmf_subsystem_add_ns nqn.2022-02.io.spdk:cnode0 Malloc0
./scripts/rpc.py nvmf_subsystem_add_listener -t ub -a 141.61.90.129 -s 4420 nqn.2022-02.io.spdk:cnode0
