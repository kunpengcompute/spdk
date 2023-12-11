#!/usr/bin/env python3

import argparse
import logging
import os
import rpc
import subprocess
import sys
from distutils.version import LooseVersion
from rpc.client import print_dict, JSONRPCException


need_reload = False
try:
    import prettytable
    if LooseVersion(prettytable.__version__) < LooseVersion('3.7.0'):
        need_reload = True
        print('version of prettytable lower than 3.7.0')
        raise ImportError('version of prettytable lower than 3.7.0')
except ImportError:
    import pip
    pip.main(['install', 'prettytable>=3.7.0'])
    if need_reload:
        print("re-run this script after prettytable upgrade")
        exit(0)
    else:
        import prettytable

# we need use rpc.py directly when arguments is not bdev_get_iostat or bdev_ocf_get_stats.
# script will exit in error function when argparse parse error. so it has to be rewritten
class LavaArgumentParser(argparse.ArgumentParser):
    def error(self, message):
        raise argparse.ArgumentError(argument=None, message=message)


TABLE_RIGHT_ALIGNMENT = 'r'
TABLE_LEFT_ALIGNMENT = 'l'

if __name__ == "__main__":
    parser = LavaArgumentParser(
        description='SPDK RPC command line interface.  For details about all options, see rpc.py.', usage='%(prog)s [options]')
        parser.add_argument('-s', dest='server_addr',
                            help='RPC domain socket path or IP address', default='/var/tmp/spdk.sock')
        parser.add_argument('-p', dest='port',
                            help='RPC port number (if server_addr is IP address)',
                            default=5260, type=int)
        parser.add_argument('-t', dest='timeout',
                            help='Timeout as a floating point number expressed in seconds waiting for response. Default: 60.0',
                            default=60.0, type=float)
        parser.add_argument('-r', dest='conn_retries',
                            help='Retry connecting to the RPC server N times with 0.2s interval. Default: 0',
                            default=0, type=int)
        parser.add_argument('-v', dest='verbose', action='store_const', const="INFO",
                            help='Set verbose mode to INFO', default="ERROR")
        parser.add_argument('--verbose', dest='verbose', choices=['DEBUG', 'INFO', 'ERROR'],
                            help="""Set verbose level. """)
        subparsers = parser.add_subparsers(help='RPC methods', dest='called_rpc_name', metavar='')

        # get stats of ocf
        def bdev_ocf_get_stats(args):
            stat = rpc.bdev.bdev_ocf_get_stats(args.client,
                                                name=args.name)
            if not stat:
                print("empty object")
                exit(1)
            
            print_order = ['usage', 'requests', 'blocks', 'debug_io', 'errors']
            group = {'usage': [['occupancy', 'free', 'clean', 'dirty']],
                    'requests': [['rd_hits', 'rd_partial_misses', 'rd_full_misses', 'rd_total', 'rd_pt'],
                                 ['wr_hits', 'wr_partial_misses', 'wr_full_misses', 'wr_total', 'wr_pt'],
                                 ['pf_partial_misses', 'pf_full_miss', 'pf_total', 'pf_pt'],
                                 ['serviced', 'total']],
                    'blocks': [['core_volume_rd', 'core_volume_wr', 'core_volume_total'],
                               ['cache_volume_rd', 'cache_volume_wr', 'cache_volume_total'],
                               ['volume_rd', 'volume_wr', 'volume_total'],
                               ['prefetch_total'],
                               ['das_limit_io_total']],
                    'debug_io': [['entry_rd', 'entry_wr']],
                    'errors': [['core_volume_rd', 'core_volume_wr', 'core_volume_total'],
                               ['cache_volume_rd', 'cache_volume_wr', 'cache_volume_total'],
                               ['total']]}
            for key in print_order:
                value = group[key]
                table = prettytable.PrettyTable()
                table.set_style(prettytable.DOUBLE_BORDER)
                table.field_names = [f'{key} statistics', 'count', '%', 'units']
                # set align style
                table.align[key+' statistics'] = TABLE_LEFT_ALIGNMENT
                table.align['count'] = TABLE_RIGHT_ALIGNMENT
                table.align['%'] = TABLE_RIGHT_ALIGNMENT
                table.align['units'] = TABLE_LEFT_ALIGNMENT
                for metrics in value:
                    for idx, metric_name in enumerate(metrics):
                        if (idx == len(metrics)-1):
                            table.add_row([metric_name, stat[key][metric_name]['count'],
                                        stat[key][metric_name]['percentage'], stat[key][metric_name]['units']], divider=True)
                        else:
                            table.add_row([metric_name, stat[key][metric_name]['count'],
                                        stat[key][metric_name]['percentage'], stat[key][metric_name]['units']], divider=False)
                print(table, '\n')

    ocf_stats_parser = subparsers.add_parser('bdev_ocf_get_stats', aliases=['get_ocf_stats'],
                            help='Get statistics of chosen OCF block device')
    ocf_stats_parser.add_argument('name', help='Name of OCF bdev')
    ocf_stats_parser.set_defaults(func=bdev_ocf_get_stats)

    # reset stats of ocf
    def bdev_ocf_reset_stats(args):
        print_dict(rpc.bdev.bdev_ocf_reset_stats(args.client,
                                                name=args.name))
    ocf_stats_parser = subparsers.add_parser('bdev_ocf_reset_stats', help='Reset statistics of chosen OCF block device')
    ocf_stats_parser.add_argument('name', help='Name of OCF bdev')
    ocf_stats_parser.set_defaults(func=bdev_ocf_reset_stats)

    # get stats of bdev io
    def bdev_get_iostat(args):
        stat = rpc.bdev.bdev_get_iostat(args.client,
                                        name=args.name)
        if not stat:
            print("empty object")
            exit(1)
        brief = ['tick_rate', 'ticks']
        group = [['bytes_read', 'num_read_ops', 'read_latency_ticks',
                  'read_latency_ticks_min', 'read_latency_ticks_max', 'read_latency_ticks_avg'],
                 ['bytes_written', 'num_write_ops', 'write_latency_ticks',
                  'write_latency_ticks_min', 'write_latency_ticks_max', 'write_latency_ticks_avg'],
                 ['bytes_unmapped', 'num_unmap_ops', 'unmap_latency_ticks'],
                 ['debug_submit_io', 'debug_retry_io', 'debug_failed_io', 'debug_abort_io']]
        # print brief
        table = prettytable.PrettyTable()
        table.border = False
        table.header = False
        table.align = TABLE_LEFT_ALIGNMENT
        for metric in brief:
            table.add_row([metric, stat[metric]])
        print(table)
        # print detail
        for metric in stat["bdevs"]:
            table = prettytable.PrettyTable()
            table.set_style(prettytable.DOUBLE_BORDER)
            table.header = False
            table.align = TABLE_LEFT_ALIGNMENT
            table.title = metric["name"]
            for metric_group in group:
                for idx, metric_name in enumerate(metric_group):
                    if (idx == len(metric_group)-1):
                        table.add_row([metric_name, metric[metric_name]], divider=True)
                    else:
                        table.add_row([metric_name, metric[metric_name]], divider=False)
                print(table, '\n')
        
        iostat_parser = subparsers.add_parser('bdev_get_iostat', aliases=['get_bdevs_iostat'],
                                    help='Display current I/O statistics of all the blockdevs or required blockdev.')
        iostat_parser.add_argument('-b', '--name', help="Name of the Blockdev. Example: Nvme0n1", required=False)
        iostat_parser.set_defaults(func=bdev_get_iostat)

        # reset stats of bdev io
        def bdev_reset_iostat(args):
            print_dict(rpc.bdev.bdev_reset_iostat(args.client,
                                                name=args.name))

        iostat_parser = subparsers.add_parser('bdev_reset_iostat', aliases=['reset_bdevs_iostat'],
                                  help='Reset current I/O statistics of all the blockdevs or required blockdev.')
        iostat_parser.add_argument('-b', '--name', help="Name of the Blockdev. Example: Nvme0n1", required=False)
        iostat_parser.set_defaults(func=bdev_reset_iostat)

        # query status of nvme or module
        def bdev_query_status(args):
            print_dict(rpc.bdev.bdev_query_status(args.client,
                                                name=args.name))
        p = subparsers.add_parser('bdev_query_status', help='Query state of nvme or ocf module or das module')
        p.add_argument('name', help='das or nvme or ocf', choices=['das', 'nvme', 'ocf'])
        p.set_defaults(func=bdev_query_status)

        use_rpc_py_directly = False
        try:
            args = parser.parse_args()
        except argparse.ArgumentError as ex:
            # unsupported choice
            if 'invalid choice' in ex.message:
                use_rpc_py_directly = True
            else:
                # supported choice but argument error
                print(ex.message)
                exit(1)

        if use_rpc_py_directly:
            rpc_path = os.path.join(os.path.split(os.path.realpath(__file__))[0], "rpc.py")
            cmd = [rpc_path] + sys.argv[1:]
            p = subprocess.run(cmd)
        elif hasattr(args, 'func'):
            try:
                args.client = rpc.client.JSONRPCClient(args.server_addr, args.port, args.timeout,
                                                        log_level=getattr(logging, args.verbose.upper()),
                                                        conn_retries=args.conn_retries)
                args.func(args)
            except JSONRPCException as ex:
                print(ex.message)
                exit(1)
        else:
            print("invalid arguments")
            parser.print_help()
            exit(1)