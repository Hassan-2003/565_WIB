# Copyright (c) 2012-2013 ARM Limited
# All rights reserved.
#
# The license below extends only to copyright in the software and shall
# not be construed as granting a license to any other intellectual
# property including but not limited to intellectual property relating
# to a hardware implementation of the functionality of the software
# licensed hereunder.  You may use the software subject to the license
# terms below provided that you ensure that this notice is replicated
# unmodified and in its entirety in all distributions of the software,
# modified or unmodified, in source code or in binary form.
#
# Copyright (c) 2006-2008 The Regents of The University of Michigan
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

# Gem5 Config Script with Configurable Decode Width and Commit Width
#
# Usage: "m5 spec_se_width_config.py --decode-width=X --commit-width=Y"

import argparse
import sys
import os

import m5
from m5.defines import buildEnv
from m5.objects import *
from m5.params import NULL
from m5.util import addToPath, fatal, warn

addToPath('../')

from ruby import Ruby

from common import Options
from common import Simulation
from common import CacheConfig
from common import CpuConfig
from common import ObjectList
from common import MemConfig
from common.FileSystemConfig import config_filesystem
from common.Caches import *
from common.cpu2000 import *

import spec2k6_spec2k17

# Import functional unit classes for custom FU pool creation  
from m5.objects.FuncUnit import *
from m5.objects.FUPool import *

def get_processes(args):
    """Interprets provided args and returns a list of processes"""

    multiprocesses = []
    inputs = []
    outputs = []
    errouts = []
    pargs = []

    workloads = args.cmd.split(';')
    if args.input != "":
        inputs = args.input.split(';')
    if args.output != "":
        outputs = args.output.split(';')
    if args.errout != "":
        errouts = args.errout.split(';')
    if args.options != "":
        pargs = args.options.split(';')

    idx = 0
    for wrkld in workloads:
        process = Process(pid = 100 + idx)
        process.executable = wrkld
        process.cwd = os.getcwd()
        process.gid = os.getgid()

        if args.env:
            with open(args.env, 'r') as f:
                process.env = [line.rstrip() for line in f]

        if len(pargs) > idx:
            process.cmd = [wrkld] + pargs[idx].split()
        else:
            process.cmd = [wrkld]

        if len(inputs) > idx:
            process.input = inputs[idx]
        if len(outputs) > idx:
            process.output = outputs[idx]
        if len(errouts) > idx:
            process.errout = errouts[idx]

        multiprocesses.append(process)
        idx += 1

    if args.smt:
        assert(args.cpu_type == "DerivO3CPU")
        return multiprocesses, idx
    else:
        return multiprocesses, 1

# Setup argument parser
parser = argparse.ArgumentParser(description='Gem5 SPEC config with configurable decode/commit widths')
Options.addCommonOptions(parser)
Options.addSEOptions(parser)

if '--ruby' in sys.argv:
    Ruby.define_options(parser)

parser.add_argument("-b", "--benchmark", default="",
                 help="The benchmark to be loaded.")

# Add comprehensive O3CPU configuration arguments based on Table 1
# Pipeline Width Parameters
parser.add_argument("--decode-width", type=int, default=8,
                    help="Decode width for the CPU (default: 8)")
parser.add_argument("--commit-width", type=int, default=8,
                    help="Commit width for the CPU (default: 8)")
parser.add_argument("--fetch-width", type=int, default=8,
                    help="Fetch width for the CPU (default: 8)")
parser.add_argument("--issue-width", type=int, default=12,
                    help="Issue width for the CPU (default: 12)")
parser.add_argument("--rename-width", type=int, default=8,
                    help="Rename width for the CPU (default: 8)")
parser.add_argument("--squash-width", type=int, default=8,
                    help="Squash width for the CPU (default: 8)")
parser.add_argument("--dispatch-width", type=int, default=8,
                    help="Dispatch width for the CPU (default: 8)")
parser.add_argument("--writeback-width", type=int, default=8,
                    help="Writeback width for the CPU (default: 8)")

# Queue Size Parameters
parser.add_argument("--rob-size", type=int, default=128,
                    help="Reorder Buffer (Active List) size (default: 128)")
parser.add_argument("--iq-size", type=int, default=32,
                    help="Integer Issue Queue size (default: 32)")
parser.add_argument("--fp-iq-size", type=int, default=32,
                    help="Floating Point Issue Queue size (default: 32)")
parser.add_argument("--lsq-size", type=int, default=64,
                    help="Load/Store Queue size (default: 64)")
parser.add_argument("--fetch-queue-size", type=int, default=8,
                    help="Instruction Fetch Queue size (default: 8)")

# Register File Parameters
parser.add_argument("--int-regs", type=int, default=128,
                    help="Number of integer physical registers (default: 128)")
parser.add_argument("--fp-regs", type=int, default=128,
                    help="Number of floating point physical registers (default: 128)")

# Functional Unit Parameters
parser.add_argument("--int-alus", type=int, default=8,
                    help="Number of integer ALUs (default: 8)")
parser.add_argument("--int-mult-div", type=int, default=2,
                    help="Number of integer multiplier/divider units (default: 2)")
parser.add_argument("--fp-alus", type=int, default=4,
                    help="Number of floating point ALUs (default: 4)")
parser.add_argument("--fp-mult-div", type=int, default=2,
                    help="Number of floating point multiplier/divider units (default: 2)")

# Memory System Parameters - Modified per request (half of Table 1 sizes)
parser.add_argument("--l1d-size", type=str, default="16kB",
                    help="L1 Data Cache size (default: 16kB, half of Table 1)")
parser.add_argument("--l1i-size", type=str, default="16kB", 
                    help="L1 Instruction Cache size (default: 16kB, half of Table 1)")
parser.add_argument("--l2-size", type=str, default="128kB",
                    help="L2 Cache size (default: 128kB, half of Table 1)")
parser.add_argument("--l1-assoc", type=int, default=4,
                    help="L1 Cache associativity (default: 4)")
parser.add_argument("--l2-assoc", type=int, default=4,
                    help="L2 Cache associativity (default: 4)")

# Prefetch Parameters - Use existing Options.py prefetcher options
# Note: --l1i-hwp-type, --l1d-hwp-type, --l2-hwp-type are already defined in Options.py
parser.add_argument("--disable-prefetch", action='store_true', default=True,
                    help="Disable hardware prefetching by setting all prefetcher types to None (default: True)")

# Store-Wait Table
parser.add_argument("--store-wait-table-size", type=int, default=2048,
                    help="Store-Wait Table size (default: 2048)")

# Branch Predictor Parameters  
parser.add_argument("--enable-bimodal-bp", action='store_true',
                    help="Enable bimodal branch predictor")
parser.add_argument("--enable-tournament-bp", action='store_true',
                    help="Enable tournament (two-level adaptive) branch predictor")

# Fast-forward and Simulation Control Parameters
parser.add_argument("--ff-instructions", type=int, default=0,
                    help="Number of instructions to fast-forward before detailed simulation (default: 0)")
parser.add_argument("--sim-instructions", type=int, default=0,
                    help="Number of instructions to simulate in detail after fast-forward (default: 0, means no limit)")
parser.add_argument("--enable-fast-forward", action='store_true',
                    help="Enable fast-forward functionality with 1B fast-forward + 1M simulation")
parser.add_argument("--measure-ff-time", action='store_true',
                    help="Measure and report fast-forward execution time")

args = parser.parse_args()

# Handle fast-forward configuration
if args.enable_fast_forward:
    args.ff_instructions  = 1000000000  # 1B instructions
    args.sim_instructions = 100000000    # 100M instructions after fast-forward
    print(f"Fast-forward mode enabled: {args.ff_instructions:,} instructions fast-forward, {args.sim_instructions:,} instructions simulation")
elif args.ff_instructions > 0:
    print(f"Custom fast-forward: {args.ff_instructions:,} instructions fast-forward, {args.sim_instructions:,} instructions simulation")

# Set gem5's built-in fast-forward and maxinsts parameters properly
if args.ff_instructions > 0:
    # gem5's --fast-forward expects a string format
    args.fast_forward = str(args.ff_instructions)
    
    # For maxinsts, we only set the ADDITIONAL detailed instructions after fast-forward
    # gem5 will fast-forward to the specified point, then run maxinsts more instructions in detailed mode
    if args.sim_instructions > 0:
        args.maxinsts = args.sim_instructions  # Only the detailed simulation instructions
        print(f"Fast-forward: {args.ff_instructions:,} instructions, then detailed simulation for {args.sim_instructions:,} more instructions")
        print(f"Detailed simulation instruction limit: {args.maxinsts:,}")
    else:
        # If no detailed simulation specified, just fast-forward
        args.maxinsts = 1  # Minimal detailed simulation
        print(f"Fast-forward only: {args.ff_instructions:,} instructions with minimal detailed simulation")
elif args.sim_instructions > 0:
    # Pure O3CPU simulation without fast-forward
    args.maxinsts = args.sim_instructions
    print(f"Pure O3CPU simulation: {args.sim_instructions:,} instructions (no fast-forward)")

# for glibcxx_3.4.20 and libgfotran.5.so
args.redirects = ['/lib64=/package/gcc/8.3.0/lib64']

# Define custom functional units with Table 1 latencies
class Table1IntALU(FUDesc):
    def __init__(self, int_alus):
        super(Table1IntALU, self).__init__()
        self.opList = [ OpDesc(opClass='IntAlu', opLat=1) ]
        self.count = int_alus
        
class Table1IntMultDiv(FUDesc):
    def __init__(self, int_mult_div):
        super(Table1IntMultDiv, self).__init__()
        self.opList = [ OpDesc(opClass='IntMult', opLat=7),
                       OpDesc(opClass='IntDiv', opLat=7, pipelined=False) ]
        self.count = int_mult_div
        
class Table1FP_ALU(FUDesc):
    def __init__(self, fp_alus):
        super(Table1FP_ALU, self).__init__()
        self.opList = [ OpDesc(opClass='FloatAdd', opLat=4),
                       OpDesc(opClass='FloatCmp', opLat=4),
                       OpDesc(opClass='FloatCvt', opLat=4) ]
        self.count = fp_alus
        
class Table1FP_MultDiv(FUDesc):
    def __init__(self, fp_mult_div):
        super(Table1FP_MultDiv, self).__init__()
        self.opList = [ OpDesc(opClass='FloatMult', opLat=4),
                       OpDesc(opClass='FloatMultAcc', opLat=4),
                       OpDesc(opClass='FloatMisc', opLat=4),
                       OpDesc(opClass='FloatDiv', opLat=12, pipelined=False),
                       OpDesc(opClass='FloatSqrt', opLat=24, pipelined=False) ]
        self.count = fp_mult_div

# Create custom FU pool with Table 1 specifications
class Table1FUPool(FUPool):
    def __init__(self, int_alus, int_mult_div, fp_alus, fp_mult_div):
        super(Table1FUPool, self).__init__()
        self.FUList = [ Table1IntALU(int_alus), Table1IntMultDiv(int_mult_div), 
                       Table1FP_ALU(fp_alus), Table1FP_MultDiv(fp_mult_div), 
                       ReadPort(), WritePort(), RdWrPort(), IprPort() ]

multiprocesses = []
numThreads = 1

process = spec2k6_spec2k17.get_process(args, buildEnv['TARGET_ISA'])
multiprocesses.append(process)

(CPUClass, test_mem_mode, FutureClass) = Simulation.setCPUClass(args)
CPUClass.numThreads = numThreads

# Define callback to configure switch CPUs with Table 1 specifications
def configure_table1_cpu(cpu, cpu_index):
    """Callback to configure O3CPU switch CPUs with Table 1 specifications"""
    print(f"Applying Table 1 specifications to switch CPU {cpu_index}...")
    
    # Pipeline Width Parameters
    if hasattr(cpu, 'decodeWidth'):
        cpu.decodeWidth = args.decode_width
    if hasattr(cpu, 'commitWidth'):
        cpu.commitWidth = args.commit_width
    if hasattr(cpu, 'fetchWidth'):
        cpu.fetchWidth = args.fetch_width
    if hasattr(cpu, 'issueWidth'):
        cpu.issueWidth = args.issue_width
    if hasattr(cpu, 'renameWidth'):
        cpu.renameWidth = args.rename_width
    if hasattr(cpu, 'squashWidth'):
        cpu.squashWidth = args.squash_width
    if hasattr(cpu, 'dispatchWidth'):
        cpu.dispatchWidth = args.dispatch_width
    if hasattr(cpu, 'wbWidth'):
        cpu.wbWidth = args.writeback_width
    
    # Queue Size Parameters
    if hasattr(cpu, 'numROBEntries'):
        cpu.numROBEntries = args.rob_size
    if hasattr(cpu, 'numIQEntries'):
        cpu.numIQEntries = args.iq_size
    if hasattr(cpu, 'LQEntries'):
        cpu.LQEntries = args.lsq_size
    if hasattr(cpu, 'SQEntries'):
        cpu.SQEntries = args.lsq_size
    if hasattr(cpu, 'fetchQueueSize'):
        cpu.fetchQueueSize = args.fetch_queue_size
    
    # Register File Parameters
    if hasattr(cpu, 'numPhysIntRegs'):
        cpu.numPhysIntRegs = args.int_regs
    if hasattr(cpu, 'numPhysFloatRegs'):
        cpu.numPhysFloatRegs = args.fp_regs
    
    # Store-Wait Table Configuration
    if hasattr(cpu, 'SSITSize'):
        cpu.SSITSize = args.store_wait_table_size
    if hasattr(cpu, 'LFSTSize'):
        cpu.LFSTSize = args.store_wait_table_size
    
    # Functional Unit Configuration
    if hasattr(cpu, 'fuPool'):
        cpu.fuPool = Table1FUPool(args.int_alus, args.int_mult_div, args.fp_alus, args.fp_mult_div)
    
    print(f"  Configured switch CPU {cpu_index}: issue={cpu.issueWidth}, ROB={cpu.numROBEntries}, IQ={cpu.numIQEntries}")

# Set the callback for switch CPU configuration when fast-forward is enabled
if args.ff_instructions > 0:
    args.custom_cpu_config_callback = configure_table1_cpu



# Check -- do not allow SMT with multiple CPUs
if args.smt and args.num_cpus > 1:
    fatal("You cannot use SMT with multiple CPUs!")

np = args.num_cpus
mp0_path = multiprocesses[0].executable
system = System(cpu = [CPUClass(cpu_id=i) for i in range(np)],
                mem_mode = test_mem_mode,
                mem_ranges = [AddrRange(args.mem_size)],
                cache_line_size = args.cacheline_size)

if numThreads > 1:
    system.multi_thread = True

# Create a top-level voltage domain
system.voltage_domain = VoltageDomain(voltage = args.sys_voltage)

# Create a source clock for the system and set the clock period
system.clk_domain = SrcClockDomain(clock =  args.sys_clock,
                                   voltage_domain = system.voltage_domain)

# Create a CPU voltage domain
system.cpu_voltage_domain = VoltageDomain()

# Create a separate clock domain for the CPUs
system.cpu_clk_domain = SrcClockDomain(clock = args.cpu_clock,
                                       voltage_domain =
                                       system.cpu_voltage_domain)

# If elastic tracing is enabled, then configure the cpu and attach the elastic
# trace probe
if args.elastic_trace_en:
    CpuConfig.config_etrace(CPUClass, system.cpu, args)

# All cpus belong to a common cpu_clk_domain, therefore running at a common
# frequency.
for cpu in system.cpu:
    cpu.clk_domain = system.cpu_clk_domain

if ObjectList.is_kvm_cpu(CPUClass) or ObjectList.is_kvm_cpu(FutureClass):
    if buildEnv['TARGET_ISA'] == 'x86':
        system.kvm_vm = KvmVM()
        system.m5ops_base = 0xffff0000
        for process in multiprocesses:
            process.useArchPT = True
            process.kvmInSE = True
    else:
        fatal("KvmCPU can only be used in SE mode with x86")

# Sanity check
if args.simpoint_profile:
    if not ObjectList.is_noncaching_cpu(CPUClass):
        fatal("SimPoint/BPProbe should be done with an atomic cpu")
    if np > 1:
        fatal("SimPoint generation not supported with more than one CPUs")


for i in range(np):
    if args.smt:
        system.cpu[i].workload = multiprocesses
    elif len(multiprocesses) == 1:
        system.cpu[i].workload = multiprocesses[0]
    else:
        system.cpu[i].workload = multiprocesses[i]

    if args.simpoint_profile:
        system.cpu[i].addSimPointProbe(args.simpoint_interval)

    if args.checker:
        system.cpu[i].addCheckerCpu()

    if args.bp_type:
        bpClass = ObjectList.bp_list.get(args.bp_type)
        system.cpu[i].branchPred = bpClass()

    if args.indirect_bp_type:
        indirectBPClass = \
            ObjectList.indirect_bp_list.get(args.indirect_bp_type)
        system.cpu[i].branchPred.indirectBranchPred = indirectBPClass()

    # Configure O3CPU parameters if this is an O3CPU (not during fast-forward)
    if hasattr(system.cpu[i], 'decodeWidth'):
        # Apply Table 1 specifications to O3CPU instance
        system.cpu[i].decodeWidth = args.decode_width
        system.cpu[i].commitWidth = args.commit_width  
        system.cpu[i].fetchWidth = args.fetch_width
        system.cpu[i].issueWidth = args.issue_width
        system.cpu[i].renameWidth = args.rename_width
        system.cpu[i].squashWidth = args.squash_width
        system.cpu[i].dispatchWidth = args.dispatch_width
        system.cpu[i].wbWidth = args.writeback_width
        system.cpu[i].numROBEntries = args.rob_size
        system.cpu[i].numIQEntries = args.iq_size
        system.cpu[i].LQEntries = args.lsq_size
        system.cpu[i].SQEntries = args.lsq_size
        system.cpu[i].fetchQueueSize = args.fetch_queue_size
        system.cpu[i].numPhysIntRegs = args.int_regs
        system.cpu[i].numPhysFloatRegs = args.fp_regs
        system.cpu[i].SSITSize = args.store_wait_table_size
        system.cpu[i].LFSTSize = args.store_wait_table_size
        system.cpu[i].fuPool = Table1FUPool(args.int_alus, args.int_mult_div, args.fp_alus, args.fp_mult_div)

    system.cpu[i].createThreads()

if args.ruby:
    Ruby.create_system(args, False, system)
    assert(args.num_cpus == len(system.ruby._cpu_ports))

    system.ruby.clk_domain = SrcClockDomain(clock = args.ruby_clock,
                                        voltage_domain = system.voltage_domain)
    for i in range(np):
        ruby_port = system.ruby._cpu_ports[i]

        # Create the interrupt controller and connect its ports to Ruby
        # Note that the interrupt controller is always present but only
        # in x86 does it have message ports that need to be connected
        system.cpu[i].createInterruptController()

        # Connect the cpu's cache ports to Ruby
        ruby_port.connectCpuPorts(system.cpu[i])
else:
    MemClass = Simulation.setMemClass(args)
    system.membus = SystemXBar()
    system.system_port = system.membus.cpu_side_ports
    
    # Detect which cache parameters were explicitly provided on command line
    command_line_args = ' '.join(sys.argv)
    user_provided_l1d_size = '--l1d_size' in command_line_args or '--l1d-size' in command_line_args
    user_provided_l1i_size = '--l1i_size' in command_line_args or '--l1i-size' in command_line_args
    user_provided_l2_size = '--l2_size' in command_line_args or '--l2-size' in command_line_args
    user_provided_l1d_assoc = '--l1d_assoc' in command_line_args or '--l1d-assoc' in command_line_args
    user_provided_l1i_assoc = '--l1i_assoc' in command_line_args or '--l1i-assoc' in command_line_args
    user_provided_l2_assoc = '--l2_assoc' in command_line_args or '--l2-assoc' in command_line_args
    
    cache_defaults_applied = False
    
    # Apply our custom defaults only if user didn't provide explicit values
    if not user_provided_l1d_size:
        args.l1d_size = "16kB"  # Half of Table 1 specification
        cache_defaults_applied = True
        
    if not user_provided_l1i_size: 
        args.l1i_size = "16kB"  # Half of Table 1 specification
        cache_defaults_applied = True
        
    if not user_provided_l2_size:
        args.l2_size = "128kB"  # Half of Table 1 specification
        cache_defaults_applied = True
        
    if not user_provided_l1d_assoc:
        args.l1d_assoc = 4  # Table 1 specification
        cache_defaults_applied = True
        
    if not user_provided_l1i_assoc:
        args.l1i_assoc = 4  # Table 1 specification
        cache_defaults_applied = True
        
    if not user_provided_l2_assoc:
        args.l2_assoc = 4  # Table 1 specification
        cache_defaults_applied = True
        
    # Ensure L2 cache is enabled when using our custom defaults
    # Force enable L2 cache for complete memory hierarchy 
    args.l2cache = True
        
    if cache_defaults_applied:
        print("Applied custom cache defaults: L1D=16kB/4-way, L1I=16kB/4-way, L2=128kB/4-way")
    else:
        print("Using user-provided cache parameters")
    
    # Set cache latencies to match table specifications
    # L1 Latency: 2 Cycles, L2 Latency: 9 Cycles (custom modification)
    args.l1_latency = '3'
    args.l2_latency = '9'
    
    # Note: L2 latency will be overridden after CacheConfig.config_cache() is called
    
    # Handle prefetch settings - Options.py already provides l1i_hwp_type, l1d_hwp_type, l2_hwp_type
    # with default=None, so we override them only if disable_prefetch is explicitly set
    if args.disable_prefetch:
        # Explicitly disable all prefetchers by setting to None
        args.l1i_hwp_type = None
        args.l1d_hwp_type = None 
        args.l2_hwp_type = None

    
    CacheConfig.config_cache(args, system)
    
    # Override L2 cache latencies to 9 cycles (custom modification, default L2Cache class uses 20 cycles)
    if hasattr(system, 'l2'):
        system.l2.tag_latency = 9
        system.l2.data_latency = 9  
        system.l2.response_latency = 9
    else:
        print("Warning: No L2 cache found to configure latencies")
    
    # Override L1 cache latencies to 3 cycles (must be done AFTER L2 to avoid conflicts)
    for i in range(np):
        if hasattr(system.cpu[i], 'dcache'):
            print(f"Overriding L1 cache latencies to 3 cycles for CPU {i}...")
            system.cpu[i].dcache.tag_latency = 3
            system.cpu[i].dcache.data_latency = 3
            system.cpu[i].dcache.response_latency = 3
            print(f"L1D cache latencies set: tag=3, data=3, response=3 cycles")
        if hasattr(system.cpu[i], 'icache'):
            system.cpu[i].icache.tag_latency = 3
            system.cpu[i].icache.data_latency = 3
            system.cpu[i].icache.response_latency = 3
            print(f"L1I cache latencies set: tag=3, data=3, response=3 cycles")
    
    # Configure memory latency to 400 CPU cycles per request
    # CPU runs at 2GHz (500ps period), so 400 cycles = 200ns total
    # Memory controller: frontend + backend = 200ns total â†’ 100ns each
    args.mem_latency = '200ns'
    
    MemConfig.config_mem(args, system)
    
    # Override memory controller latencies to ensure 400 CPU cycles (200ns total at 2GHz)
    # Note: Memory controller runs on system clock (1GHz), but latency is absolute time
    # 400 CPU cycles @ 2GHz = 200ns = 100ns frontend + 100ns backend
    if hasattr(system, 'mem_ctrls'):
        print("Overriding memory controller latencies to 400 CPU cycles (200ns @ 2GHz)...")
        for mem_ctrl in system.mem_ctrls:
            # Set static latencies: 100ns frontend + 100ns backend = 200ns total = 400 CPU cycles
            if hasattr(mem_ctrl, 'static_frontend_latency'):
                mem_ctrl.static_frontend_latency = '100ns'
            if hasattr(mem_ctrl, 'static_backend_latency'): 
                mem_ctrl.static_backend_latency = '100ns'
            # For DRAM interfaces, also set the interface latency
            if hasattr(mem_ctrl, 'dram'):
                if hasattr(mem_ctrl.dram, 'latency'):
                    mem_ctrl.dram.latency = '200ns'
        print(f"Memory controller latencies set: frontend=100ns, backend=100ns (total=200ns = 400 CPU cycles @ 2GHz)")
    else:
        print("Warning: No memory controllers found to configure latencies")
        
    config_filesystem(system, args)

system.workload = SEWorkload.init_compatible(mp0_path)

if args.wait_gdb:
    system.workload.wait_for_remote_gdb = True



root = Root(full_system = False, system = system)

# Handle fast-forward execution with timing measurement
if args.ff_instructions > 0 and args.measure_ff_time:
    import time
    print(f"\nStarting fast-forward execution of {args.ff_instructions:,} instructions...")
    start_time = time.time()
    
    # Create a simple CPU for fast-forward if needed
    # gem5 will handle this automatically through the built-in fast-forward mechanism
    print(f"Fast-forward target: {args.ff_instructions:,} instructions")
    if args.sim_instructions > 0:
        print(f"Detailed simulation target: {args.sim_instructions:,} instructions")
    
    # Run simulation with built-in fast-forward
    exit_event = Simulation.run(args, root, system, FutureClass)
    
    end_time = time.time()
    ff_duration = end_time - start_time
    
    print(f"\nFast-forward execution completed!")
    print(f"Total execution time: {ff_duration:.2f} seconds")
    print(f"Fast-forward rate: {args.ff_instructions / ff_duration:,.0f} instructions/second")
    if args.sim_instructions > 0:
        print(f"Total simulated instructions: {args.ff_instructions + args.sim_instructions:,}")
else:
    # Standard execution without timing measurement
    if args.ff_instructions > 0:
        print(f"\nExecuting with fast-forward: {args.ff_instructions:,} instructions")
        if args.sim_instructions > 0:
            print(f"Detailed simulation: {args.sim_instructions:,} instructions")
    
    Simulation.run(args, root, system, FutureClass)