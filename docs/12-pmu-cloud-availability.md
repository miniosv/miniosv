## Performance Monitoring resources available in the Cloud
Cloud providers do not necessarily expose all performance monitoring units and counters to the virtual machine guest.
This may be to run their own monitoring on the remaining counters, or (especially for uncore PMUs) simply a lack of support in the hypervisor.

### Amazon Web Services
The following section presents findings on PMU availability and performance monitoring counters for AWS virtual machines running on AWS Nitro. Note that AWS provides some public information on this [here](https://github.com/aws/aperf/blob/main/docs/PMU.md).

#### Counter slicing
The CPU generally advertises a lower number of counters per core as would be available on metal machines:
- Intel: 8/8 counters
- AMD: ~5/6 counters
- ARM: 2/6 counters
> [!NOTE]
> Not only metal machines have access to the full number of core counters. As soon as the entire socket is owned by a single VM, the full set of counters can be used.
> For a list of non-metal machines that should have full PMU support, consider [this](https://github.com/aws/aws-graviton-getting-started/blob/main/perfrunbook/debug_hw_perf.md#how-to-collect-pmu-counters) aws source.

#### Uncore PMUs
According to [Intel](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-vtune-amplifier-functionality-on-aws-instances.html) and own experiments,
AWS Nitro does not expose any uncore PMU to the guest. This is consistent with current KVM behavior.

#### Edge Cases
- Smaller graviton 3 instances advertise counter 0 and 1. However, the value of counter 0 is set to `0xdeadbeef` and does not change when the counter is enabled. The counter is therefore currently disabled in software, when the corresponding microarchitecture is detected. 
