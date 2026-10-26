bw_mem_dir=$1

cd $bw_mem_dir
for cpu in $(seq 16 39); do
    nohup numactl -C $cpu -m 0 ./bw_mem -P 1 -W 1 -N 100000 16M rd &
done