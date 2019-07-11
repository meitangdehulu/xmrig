/* XMRig
 * Copyright 2010      Jeff Garzik <jgarzik@pobox.com>
 * Copyright 2012-2014 pooler      <pooler@litecoinpool.org>
 * Copyright 2014      Lucas Jones <https://github.com/lucasjones>
 * Copyright 2014-2016 Wolf9466    <https://github.com/OhGodAPet>
 * Copyright 2016      Jay D Dee   <jayddee246@gmail.com>
 * Copyright 2017-2018 XMR-Stak    <https://github.com/fireice-uk>, <https://github.com/psychocrypt>
 * Copyright 2018      Lee Clagett <https://github.com/vtnerd>
 * Copyright 2018-2019 SChernykh   <https://github.com/SChernykh>
 * Copyright 2016-2019 XMRig       <https://github.com/xmrig>, <support@xmrig.com>
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program. If not, see <http://www.gnu.org/licenses/>.
 */


#include <thread>


#include "crypto/cn/CryptoNight_test.h"
#include "crypto/common/Nonce.h"
#include "crypto/rx/Rx.h"
#include "crypto/rx/RxVm.h"
#include "net/JobResults.h"
#include "workers/CpuThreadLegacy.h"
#include "workers/MultiWorker.h"
#include "workers/Workers.h"


namespace xmrig {

static constexpr uint32_t kReserveCount = 4096;

} // namespace xmrig


template<size_t N>
xmrig::MultiWorker<N>::MultiWorker(ThreadHandle *handle)
    : Worker(handle)
{
    if (m_thread->algorithm().family() != Algorithm::RANDOM_X) {
        m_memory = Mem::create(m_ctx, m_thread->algorithm(), N);
    }
}


template<size_t N>
xmrig::MultiWorker<N>::~MultiWorker()
{
    Mem::release(m_ctx, N, m_memory);

#   ifdef XMRIG_ALGO_RANDOMX
    delete m_vm;
#   endif
}


#ifdef XMRIG_ALGO_RANDOMX
template<size_t N>
void xmrig::MultiWorker<N>::allocateRandomX_VM()
{
    if (!m_vm) {
        RxDataset *dataset = Rx::dataset(m_job.currentJob().seedHash(), m_job.currentJob().algorithm());
        m_vm = new RxVm(dataset, true, m_thread->isSoftAES());
    }
}
#endif


template<size_t N>
bool xmrig::MultiWorker<N>::selfTest()
{
    if (m_thread->algorithm().family() == Algorithm::CN) {
        const bool rc = verify(Algorithm::CN_0,      test_output_v0)   &&
                        verify(Algorithm::CN_1,      test_output_v1)   &&
                        verify(Algorithm::CN_2,      test_output_v2)   &&
                        verify(Algorithm::CN_FAST,   test_output_msr)  &&
                        verify(Algorithm::CN_XAO,    test_output_xao)  &&
                        verify(Algorithm::CN_RTO,    test_output_rto)  &&
                        verify(Algorithm::CN_HALF,   test_output_half) &&
                        verify2(Algorithm::CN_WOW,   test_output_wow)  &&
                        verify2(Algorithm::CN_R,     test_output_r)    &&
                        verify(Algorithm::CN_RWZ,    test_output_rwz)  &&
                        verify(Algorithm::CN_ZLS,    test_output_zls)  &&
                        verify(Algorithm::CN_DOUBLE, test_output_double);

#       ifdef XMRIG_ALGO_CN_GPU
        if (!rc || N > 1) {
            return rc;
        }

        return verify(Algorithm::CN_GPU, test_output_gpu);
#       else
        return rc;
#       endif
    }

#   ifdef XMRIG_ALGO_CN_LITE
    if (m_thread->algorithm().family() == Algorithm::CN_LITE) {
        return verify(Algorithm::CN_LITE_0,    test_output_v0_lite) &&
               verify(Algorithm::CN_LITE_1,    test_output_v1_lite);
    }
#   endif

#   ifdef XMRIG_ALGO_CN_HEAVY
    if (m_thread->algorithm().family() == Algorithm::CN_HEAVY) {
        return verify(Algorithm::CN_HEAVY_0,    test_output_v0_heavy)  &&
               verify(Algorithm::CN_HEAVY_XHV,  test_output_xhv_heavy) &&
               verify(Algorithm::CN_HEAVY_TUBE, test_output_tube_heavy);
    }
#   endif

#   ifdef XMRIG_ALGO_CN_PICO
    if (m_thread->algorithm().family() == Algorithm::CN_PICO) {
        return verify(Algorithm::CN_PICO_0, test_output_pico_trtl);
    }
#   endif

#   ifdef XMRIG_ALGO_RANDOMX
    if (m_thread->algorithm().family() == Algorithm::RANDOM_X) {
        return true;
    }
#   endif

    return false;
}


template<size_t N>
void xmrig::MultiWorker<N>::start()
{
    while (Nonce::sequence() > 0) {
        if (Workers::isPaused()) {
            do {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            while (Workers::isPaused());

            if (Nonce::sequence() == 0) {
                break;
            }

            consumeJob();
        }

        while (!Nonce::isOutdated(m_job.sequence())) {
            if ((m_count & 0x7) == 0) {
                storeStats();
            }

            const Job &job = m_job.currentJob();

#           ifdef XMRIG_ALGO_RANDOMX
            if (job.algorithm().family() == Algorithm::RANDOM_X) {
                allocateRandomX_VM();
                randomx_calculate_hash(m_vm->get(), m_job.blob(), job.size(), m_hash);
            }
            else
#           endif
            {
                m_thread->fn(job.algorithm())(m_job.blob(), job.size(), m_hash, m_ctx, job.height());
            }

            for (size_t i = 0; i < N; ++i) {
                if (*reinterpret_cast<uint64_t*>(m_hash + (i * 32) + 24) < job.target()) {
                    JobResults::submit(JobResult(job.poolId(), job.id(), job.clientId(), *m_job.nonce(i), m_hash + (i * 32), job.diff(), job.algorithm()));
                }
            }

            m_job.nextRound(kReserveCount);
            m_count += N;

            std::this_thread::yield();
        }

        consumeJob();
    }
}


template<size_t N>
bool xmrig::MultiWorker<N>::verify(const Algorithm &algorithm, const uint8_t *referenceValue)
{
    cn_hash_fun func = m_thread->fn(algorithm);
    if (!func) {
        return false;
    }

    func(test_input, 76, m_hash, m_ctx, 0);
    return memcmp(m_hash, referenceValue, sizeof m_hash) == 0;
}


template<size_t N>
bool xmrig::MultiWorker<N>::verify2(const Algorithm &algorithm, const uint8_t *referenceValue)
{
    cn_hash_fun func = m_thread->fn(algorithm);
    if (!func) {
        return false;
    }

    for (size_t i = 0; i < (sizeof(cn_r_test_input) / sizeof(cn_r_test_input[0])); ++i) {
        const size_t size = cn_r_test_input[i].size;
        for (size_t k = 0; k < N; ++k) {
            memcpy(m_job.blob() + (k * size), cn_r_test_input[i].data, size);
        }

        func(m_job.blob(), size, m_hash, m_ctx, cn_r_test_input[i].height);

        for (size_t k = 0; k < N; ++k) {
            if (memcmp(m_hash + k * 32, referenceValue + i * 32, sizeof m_hash / N) != 0) {
                return false;
            }
        }
    }

    return true;
}


namespace xmrig {

template<>
bool MultiWorker<1>::verify2(const Algorithm &algorithm, const uint8_t *referenceValue)
{
    cn_hash_fun func = m_thread->fn(algorithm);
    if (!func) {
        return false;
    }

    for (size_t i = 0; i < (sizeof(cn_r_test_input) / sizeof(cn_r_test_input[0])); ++i) {
        func(cn_r_test_input[i].data, cn_r_test_input[i].size, m_hash, m_ctx, cn_r_test_input[i].height);

        if (memcmp(m_hash, referenceValue + i * 32, sizeof m_hash) != 0) {
            return false;
        }
    }

    return true;
}

} // namespace xmrig


template<size_t N>
void xmrig::MultiWorker<N>::consumeJob()
{
    m_job.add(Workers::job(), Nonce::sequence(), kReserveCount);
}


namespace xmrig {

template class MultiWorker<1>;
template class MultiWorker<2>;
template class MultiWorker<3>;
template class MultiWorker<4>;
template class MultiWorker<5>;

} // namespace xmrig

