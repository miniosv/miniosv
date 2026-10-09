## Performance Monitoring

MiniOSv leverages hardware performance counters directly. The corresponding architecture-specific implementation can be found in `arch/<arch>/arch-perf.hh`.

> [!NOTE]
> When running virtualized, these counters are gated. A VM-exit is therefore required to query or change their contents.


### Performance Monitoring Unit (PMU)
CPU packages contain multiple units in charge or performance monitoring:
- Oncore PMUs: Each CPU core contains a unit capable of measuring oncore events (cycles, l1 cache misses, branch mispredictions, ...)
- Uncore PMU(s): Each CPU package contains one or more units capable of measuring traffic on the package (l3 cache misses, Local upstream DMA read data bytes, ...)


#### Basic Functionality
Most oncore PMUs allow counting 6 events concurrently (IDs 0-5). Counting one event requires a pair of 2 registers:

<img width="710" height="438" alt="hardware-counters" src="https://github.com/user-attachments/assets/91c1779d-1449-499e-9913-61d56d7bf8fa" />

- Performance Event Selector [0-5] (`PerfEvtSel[0-5]`)
    - Write to this register to configure which event you want to count
    - This register allows for further configuration (enable counting, interrupt on overflow, ...)
- Performance Monitoring Counter [0-5] (`PMC[0-5]`)
    - Read from this register to get the number of events counted

This conceptually maps the [PerfEvent header](https://github.com/viktorleis/perfevent) syntax to the following 4 x86 assembly instructions:

<table>
<tr><th>PerfEvent</th><th>x86 assembly</th></tr>
<tr><td>

```c++
PerfEvent e;
e.startCounters();
yourBenchmark();
e.stopCounters();
```

</td><td>

```asm
wrmsr $PerfEvtSel0, $conf
rdmsr $PMC0
call yourBenchmark
rdmsr $PMC0
```

</td></tr>
</table>


#### Interface
MiniOsv provides a low-level interface that allows driving the PMU directly from the application
```c++
// Create a virtual representation of the oncore PMU of the underlying architecture. This only works propperly for supported microarchitectures
std::vector<PMC> make_default_core_pmcs();

// Allocate a counter pair of a given class from your virtual PMU representation. The class tells which events can be measured by a specific counter (e.g. CYCLES, or CORE)
pmcs.acquire(PMClass pmClass);

// Start counting on an allocated counter with a specific event configuration and initial value for the counter
pmc.start_with_conf(uint64_t perfEvtSel, uint64_t intial_value);

// Read the current value of an allocated PMC
pmc.read();

// Write a value to an allocated PMC
pmc.write(uint64_t value);

// Stop (unset enable bit) on a specific counter. This makes sure no further overflow interrupts can fire
pmc.stop();

// Deallocate the counter pair
pmcs.release(PMC *pmc)
```

### PerfEvent
For convenience, miniOSv implements the PerfEvent abstraction on top of the low-level interface presented above. This allows linux `perf` like counting inside the unikernel. For an introduction to PerfEvent, consider the [original linux wrapper](https://github.com/viktorleis/perfevent).


### Supported Microarchitectures
As of September 2026, miniOSv's perf infrastructure supports the following microarchitectures

#### x86-64
- AMD Zen 4
- AMD Zen 5
- Intel Skylake

#### ARM
- Neoverse V1
- Neoverse V2
- Ampere 1a

To add support for more microarchitectures, [papi](https://github.com/icl-utk-edu/papi/tree/master/src/libpfm4/lib/events) and [likwid](https://github.com/RRZE-HPC/likwid/tree/master/src/includes) may be good entrypoints to get the register addresses and event codes
