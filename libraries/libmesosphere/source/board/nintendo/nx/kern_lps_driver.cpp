/*
 * Copyright (c) 2018-2020 Atmosphère-NX
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <mesosphere.hpp>
#include "kern_lps_driver.hpp"
#include "kern_k_sleep_manager.hpp"

#include "kern_bpmp_api.hpp"
#include "kern_atomics_registers.hpp"
#include "kern_ictlr_registers.hpp"
#include "kern_flow_registers.hpp"
#include "kern_sema_registers.hpp"

namespace ams::kern::board::nintendo::nx::lps {

    namespace {

        constexpr inline int ChannelCount = 12;

        constexpr inline TimeSpan ChannelTimeout = TimeSpan::FromMicroSeconds(1);

        constinit bool g_lps_init_done         = false;
        constinit bool g_bpmp_connected        = false;
        constinit bool g_bpmp_mail_initialized = false;

        constinit KSpinLock g_bpmp_mrq_lock;

        constinit KVirtualAddress g_evp_address     = Null<KVirtualAddress>;
        constinit KVirtualAddress g_flow_address    = Null<KVirtualAddress>;
        constinit KVirtualAddress g_prictlr_address = Null<KVirtualAddress>;
        constinit KVirtualAddress g_sema_address    = Null<KVirtualAddress>;
        constinit KVirtualAddress g_atomics_address = Null<KVirtualAddress>;
        constinit KVirtualAddress g_clkrst_address  = Null<KVirtualAddress>;

        constinit ChannelData g_channel_area[ChannelCount] = {};

        ALWAYS_INLINE u32 Read(KVirtualAddress address) {
            return *GetPointer<volatile u32>(address);
        }

        ALWAYS_INLINE void Write(KVirtualAddress address, u32 value) {
            *GetPointer<volatile u32>(address) = value;
        }

        void InitializeDeviceVirtualAddresses() {
            /* Retrieve randomized mappings. */
            g_evp_address     = KMemoryLayout::GetDeviceVirtualAddress(KMemoryRegionType_LegacyLpsExceptionVectors);
            g_flow_address    = KMemoryLayout::GetDeviceVirtualAddress(KMemoryRegionType_LegacyLpsFlowController);
            g_prictlr_address = KMemoryLayout::GetDeviceVirtualAddress(KMemoryRegionType_LegacyLpsPrimaryICtlr);
            g_sema_address    = KMemoryLayout::GetDeviceVirtualAddress(KMemoryRegionType_LegacyLpsSemaphore);
            g_atomics_address = KMemoryLayout::GetDeviceVirtualAddress(KMemoryRegionType_LegacyLpsAtomics);
            g_clkrst_address  = KMemoryLayout::GetDeviceVirtualAddress(KMemoryRegionType_LegacyLpsClkRst);
        }

        /* NOTE: linux "do_cc4_init" */
        void ConfigureCc3AndCc4() {
            /* Configure CC4/CC3 as enabled with time threshold as 2 microseconds. */
            Write(g_flow_address + FLOW_CTLR_CC4_HVC_CONTROL, (0x2 << 3) | 0x1);

            /* Configure Retention with threshold 2 microseconds. */
            Write(g_flow_address + FLOW_CTLR_CC4_RETENTION_CONTROL, (0x2 << 3));

            /* Configure CC3/CC3 retry threshold as 2 microseconds. */
            Write(g_flow_address + FLOW_CTLR_CC4_HVC_RETRY, (0x2 << 3));

            /* Read the retry register to ensure writes take. */
            Read(g_flow_address + FLOW_CTLR_CC4_HVC_RETRY);
        }

        constexpr bool IsValidMessageDataSize(int size) {
            return 0 <= size && size < MessageDataSizeMax;
        }

        /* NOTE: linux "bpmp_valid_txfer" */
        constexpr bool IsTransferValid(const void *ob, int ob_size, void *ib, int ib_size) {
            return IsValidMessageDataSize(ob_size) && IsValidMessageDataSize(ib_size) && (ob_size == 0 || ob != nullptr) && (ib_size == 0 || ib != nullptr);
        }

        /* NOTE: linux "bpmp_ob_channel" */
        int BpmpGetOutboundChannel() {
            return GetCurrentCoreId();
        }

        /* NOTE: linux "bpmp_ch_sta" */
        u32 BpmpGetChannelState(int channel) {
            cpu::DataSynchronizationBarrier();
            return Read(g_sema_address + RES_SEMA_SHRD_SMP_STA) & CH_MASK(channel);
        }

        /* NOTE: linux "bpmp_master_free" */
        bool BpmpIsMasterFree(int channel) {
            return BpmpGetChannelState(channel) == MA_FREE(channel);
        }

        /* NOTE: linux "bpmp_master_acked" */
        bool BpmpIsMasterAcked(int channel) {
            return BpmpGetChannelState(channel) == MA_ACKD(channel);
        }

        /* NOTE: linux "bpmp_signal_slave" */
        void BpmpSignalSlave(int channel) {
            Write(g_sema_address + RES_SEMA_SHRD_SMP_CLR, CH_MASK(channel));
            cpu::DataSynchronizationBarrier();
        }

        /* NOTE: linux "bpmp_free_master" */
        void BpmpFreeMaster(int channel) {
            /* Transition state from ack'd to free. */
            Write(g_sema_address + RES_SEMA_SHRD_SMP_CLR, ((MA_ACKD(channel)) ^ (MA_FREE(channel))));
            cpu::DataSynchronizationBarrier();
        }

        /* NOTE: linux "bpmp_ring_doorbell" */
        void BpmpRingDoorbell() {
            Write(g_prictlr_address + ICTLR_FIR_SET(INT_SHR_SEM_OUTBOX_IBF), FIR_BIT(INT_SHR_SEM_OUTBOX_IBF));
            cpu::DataSynchronizationBarrier();
        }

        /* NOTE: linux "bpmp_wait_master_free" */
        int BpmpWaitMasterFree(int channel) {
            /* Check if the master is already freed. */
            if (BpmpIsMasterFree(channel)) {
                return 0;
            }

            /* Spin-poll for the master to be freed until timeout occurs. */
            const auto start_tick = KHardwareTimer::GetTick();
            const auto timeout    = ams::svc::Tick(ChannelTimeout);
            do {
                if (BpmpIsMasterFree(channel)) {
                    return 0;
                }
            } while ((KHardwareTimer::GetTick() - start_tick) < timeout);

            /* The master didn't become free. */
            return -1;
        }

        /* NOTE: linux "bpmp_wait_ack" */
        int BpmpWaitAck(int channel) {
            /* Check if the master is already ACK'd. */
            if (BpmpIsMasterAcked(channel)) {
                return 0;
            }

            /* Spin-poll for the master to be ACK'd until timeout occurs. */
            const auto start_tick = KHardwareTimer::GetTick();
            const auto timeout    = ams::svc::Tick(ChannelTimeout);
            do {
                if (BpmpIsMasterAcked(channel)) {
                    return 0;
                }
            } while ((KHardwareTimer::GetTick() - start_tick) < timeout);

            /* The master didn't get ACK'd. */
            return -1;
        }

        /* NOTE: linux "bpmp_write_ch" */
        int BpmpWriteChannel(int channel, int mrq, int flags, const void *data, size_t data_size) {
            /* Wait to be able to master the mailbox. */
            if (int res = BpmpWaitMasterFree(channel); res != 0) {
                return res;
            }

            /* Prepare the message. */
            MailboxData *mb = g_channel_area[channel].ob;
            mb->code  = mrq;
            mb->flags = flags;
            if (data != nullptr) {
                std::memcpy(mb->data, data, data_size);
            }

            /* Signal to slave that message is available. */
            BpmpSignalSlave(channel);

            return 0;
        }

        /* NOTE: linux "__bpmp_read_ch" */
        int BpmpReadChannel(int channel, void *data, size_t data_size) {
            /* Get the message. */
            MailboxData *mb = g_channel_area[channel].ib;

            /* Copy any return data. */
            if (data != nullptr) {
                std::memcpy(data, mb->data, data_size);
            }

            /* Free the channel. */
            BpmpFreeMaster(channel);

            /* Return result. */
            return mb->code;
        }

        /* NOTE: linux "tegra_bpmp_send_receive_atomic" or "tegra_bpmp_send_receive". */
        int BpmpSendAndReceive(int mrq, const void *ob, int ob_size, void *ib, int ib_size) {
            /* Validate that the data transfer is valid. */
            if (!IsTransferValid(ob, ob_size, ib, ib_size)) {
                return -1;
            }

            /* Validate that the bpmp is connected. */
            if (!g_bpmp_connected) {
                return -1;
            }

            /* Disable interrupts. */
            KScopedInterruptDisable di;

            /* Acquire exclusive access to send mrqs. */
            KScopedSpinLock lk(g_bpmp_mrq_lock);

            /* Send the message. */
            int channel = BpmpGetOutboundChannel();
            if (int res = BpmpWriteChannel(channel, mrq, BPMP_MSG_DO_ACK, ob, ob_size); res != 0) {
                return res;
            }

            /* Send "doorbell" irq to the bpmp firmware. */
            BpmpRingDoorbell();

            /* Wait for the bpmp firmware to acknowledge our request. */
            if (int res = BpmpWaitAck(channel); res != 0) {
                return res;
            }

            /* Read the data the bpmp sent back. */
            return BpmpReadChannel(channel, ib, ib_size);
        }

        /* NOTE: linux "tegra_bpmp_send" */
        int BpmpSend(int mrq, const void *ob, int ob_size) {
            /* Validate that the data transfer is valid. */
            if (!IsTransferValid(ob, ob_size, nullptr, 0)) {
                return -1;
            }

            /* Validate that the bpmp is connected. */
            if (!g_bpmp_connected) {
                return -1;
            }

            /* Disable interrupts. */
            KScopedInterruptDisable di;

            /* Acquire exclusive access to send mrqs. */
            KScopedSpinLock lk(g_bpmp_mrq_lock);

            /* Send the message. */
            int channel = BpmpGetOutboundChannel();
            if (int res = BpmpWriteChannel(channel, mrq, 0, ob, ob_size); res != 0) {
                return res;
            }

            /* Send "doorbell" irq to the bpmp firmware. */
            BpmpRingDoorbell();

            return 0;
        }

        /* NOTE: modified linux "tegra_bpmp_enable_suspend" */
        int BpmpEnableSuspend(int mode, int flags) {
            /* Prepare data for bpmp. */
            const s32 data[] = { mode, flags };

            /* Send the data. */
            return BpmpSend(MRQ_ENABLE_SUSPEND, data, sizeof(data));
        }

        /* NOTE: linux "__bpmp_connect" */
        int ConnectToBpmp() {
            /* Check if we've already connected. */
            if (g_bpmp_connected) {
                return 0;
            }

            /* Verify that the resource semaphore state is set. */
            if (Read(g_sema_address + RES_SEMA_SHRD_SMP_STA) == 0) {
                return -1;
            }

            /* Get the channels, which the bpmp firmware has configured in advance. */
            {
                const KVirtualAddress  iram_virt_addr = KMemoryLayout::GetDeviceVirtualAddress (KMemoryRegionType_LegacyLpsIram);
                const KPhysicalAddress iram_phys_addr = KMemoryLayout::GetDevicePhysicalAddress(KMemoryRegionType_LegacyLpsIram);

                for (auto i = 0; i < ChannelCount; ++i) {
                    /* Trigger a get command for the desired channel. */
                    Write(g_atomics_address + ATOMICS_AP0_TRIGGER, TRIGGER_CMD_GET | (i << 16));

                    /* Retrieve the channel phys-addr-in-iram, and convert it to a kernel address. */
                    auto *ch = GetPointer<MailboxData>(iram_virt_addr + (Read(g_atomics_address + ATOMICS_AP0_RESULT(i)) - GetInteger(iram_phys_addr)));

                    /* Verify the channel isn't null. */
                    /* NOTE: This is an utterly nonsense check, as this would require the bpmp firmware to specify */
                    /*       a phys-to-virt diff as an address. On 1.0.0, which had no ASLR, this was 0x8028C000.  */
                    /*       However, Nintendo has the check, and we'll preserve it to be faithful.                */
                    if (ch == nullptr) {
                        return -1;
                    }

                    /* Set the channel in the channel area. */
                    g_channel_area[i].ib = ch;
                    g_channel_area[i].ob = ch;
                }
            }

            /* Mark driver as connected to bpmp. */
            g_bpmp_connected = true;

            return 0;
        }

        /* NOTE: Modified linux "bpmp_mail_init" */
        int InitializeBpmpMail() {
            /* Check if we've already initialized. */
            if (g_bpmp_mail_initialized) {
                return 0;
            }

            /* Mark function as having been called. */
            g_bpmp_mail_initialized = true;

            /* Forward declare result/reply variables. */
            int res, request = 0, reply = 0;

            /* Try to connect to the bpmp. */
            if (res = ConnectToBpmp(); res != 0) {
                MESOSPHERE_LOG("bpmp: connect error returns %d\n", res);
                return res;
            }

            /* Ensure that we can successfully ping the bpmp. */
            request = 1;
            if (res = BpmpSendAndReceive(MRQ_PING, std::addressof(request), sizeof(request), std::addressof(reply), sizeof(reply)); res != 0) {
                MESOSPHERE_LOG("bpmp: MRQ_PING error returns %d with reply %d\n", res, reply);
                return res;
            }

            /* Configure the PMIC. */
            request = 1;
            if (res = BpmpSendAndReceive(MRQ_CPU_PMIC_SELECT, std::addressof(request), sizeof(request), std::addressof(reply), sizeof(reply)); res != 0) {
                MESOSPHERE_LOG("bpmp: MRQ_CPU_PMIC_SELECT for MAX77621 error returns %d with reply %d\n", res, reply);
                return res;
            }

            return 0;
        }

    }

    void Initialize() {
        if (!g_lps_init_done) {
            /* Get the addresses of the devices the driver needs. */
            InitializeDeviceVirtualAddresses();

            /* Configure CC3/CC4. */
            ConfigureCc3AndCc4();

            /* Initialize ccplex <-> bpmp mail. */
            /* NOTE: Nintendo does not check that this call succeeds. */
            InitializeBpmpMail();

            g_lps_init_done = true;
        }
    }

    Result EnableSuspend(bool enable) {
        /* If we're not on core 0, there's nothing to do. */
        R_SUCCEED_IF(GetCurrentCoreId() != 0);

        /* If we're not enabling suspend, there's nothing to do. */
        R_SUCCEED_IF(!enable);

        /* Instruct BPMP to enable suspend-to-sc7. */
        R_UNLESS(BpmpEnableSuspend(TEGRA_BPMP_PM_SC7, 0) == 0, svc::ResultInvalidState());

        return ResultSuccess();
    }

    void InvokeCpuSleepHandler(uintptr_t arg, uintptr_t entry) {
        MESOSPHERE_ABORT_UNLESS(g_lps_init_done);
        MESOSPHERE_ABORT_UNLESS(GetCurrentCoreId() == 0);

        MESOSPHERE_UNIMPLEMENTED();

        /* Invoke the sleep hander. */
        KSleepManager::CpuSleepHandler(arg, entry);

        /* TODO: restore saved clkrst reg */

        /* Configure CC3/CC4. */
        ConfigureCc3AndCc4();
    }

}