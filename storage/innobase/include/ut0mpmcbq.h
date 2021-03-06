/*****************************************************************************
Copyright (c) 2017, 2021, Oracle and/or its affiliates.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License, version 2.0,
as published by the Free Software Foundation.

This program is also distributed with certain software (including
but not limited to OpenSSL) that is licensed under separate terms,
as designated in a particular file or component or in included license
documentation.  The authors of MySQL hereby grant you an additional
permission to link the program and your derivative works with the
separately licensed software that they have included with MySQL.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License, version 2.0, for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

#ifndef ut0mpmcbq_h
#define ut0mpmcbq_h

#include "ut0cpu_cache.h"

#include <atomic>

/** Multiple producer consumer, bounded queue
 Implementation of Dmitry Vyukov's MPMC algorithm
 http://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue */
template <typename T>
class mpmc_bq {
 public:
  /** Constructor
  @param[in]	n_elems		Max number of elements allowed */
  /* 构造函数, 传入的参数为队列的长度, 必须为 2 的倍数. */
  explicit mpmc_bq(size_t n_elems)
        /* 构造 n_elems 个元素, Cell 为元素. */
      : m_ring(reinterpret_cast<Cell *>(UT_NEW_ARRAY_NOKEY(Aligned, n_elems))),
        m_capacity(n_elems - 1) {
    /* Should be a power of 2 */
    ut_a((n_elems >= 2) && ((n_elems & (n_elems - 1)) == 0));

    for (size_t i = 0; i < n_elems; ++i) {
      /* 初始化每个元素.
       * Cell 有两个数据成员: m_data, m_pos.
       * m_pos 初始化为元素对应的索引下标, 0, 1, 2, ... */
      m_ring[i].m_pos.store(i, std::memory_order_relaxed);
    }

    /* m_enqueue_pos 代表下一个可以插入的空闲的元素索引下标, 初始化 0. */
    /* m_dequeue_pos 代表下一个可以出列的元素索引下标, 初始化 0. */
    m_enqueue_pos.store(0, std::memory_order_relaxed);
    m_dequeue_pos.store(0, std::memory_order_relaxed);
  }

  /** Destructor */
  ~mpmc_bq() { UT_DELETE_ARRAY(m_ring); }

  /** Enqueue an element
  @param[in]	data		Element to insert, it will be copied
  @return true on success */
  /* 元素入列. */
  bool enqueue(T const &data) MY_ATTRIBUTE((warn_unused_result)) {
    /* m_enqueue_pos only wraps at MAX(m_enqueue_pos), instead
    we use the capacity to convert the sequence to an array
    index. This is why the ring buffer must be a size which
    is a power of 2. This also allows the sequence to double
    as a ticket/lock. */

    /* 获取当前的 m_enqueue_pos. */
    size_t pos = m_enqueue_pos.load(std::memory_order_relaxed);

    Cell *cell;

    for (;;) {
      /* 以 m_capacity 取模求对应位置的元素. */
      cell = &m_ring[pos & m_capacity];

      size_t seq;

      /* 获取元素 cell 的 m_pos. */
      seq = cell->m_pos.load(std::memory_order_acquire);

      /* 计算 cell->m_pos 和 m_enqueue_pos 的差值. */
      intptr_t diff = (intptr_t)seq - (intptr_t)pos;

      /* If they are the same then it means this cell is empty */

      if (diff == 0) {
        /* Claim our spot by moving head. If head isn't the same as we last
        checked then that means someone beat us to the punch. Weak compare is
        faster, but can return spurious results which in this instance is OK,
        because it's in the loop */

        /* cell->m_pos 和 m_enqueue_pos 相等代表 cell 为空闲状态, m_enqueue_pos 自增 1,
         * 假如自增失败即代表当前位置的 cell 已被占用, 需要重新获取 m_enqueue_pos. */
        if (m_enqueue_pos.compare_exchange_weak(pos, pos + 1,
                                                std::memory_order_relaxed)) {
          break;
        }

      } else if (diff < 0) {
        /* The queue is full */

        /* cell->m_pos 和 m_enqueue_pos 在入列成功的状态下都是自增 1.
         * diff < 0  即代表 m_enqueue_pos 已经回环，但当前的 cell 仍未出列. */

        return (false);

      } else {
        /* 重新获取 m_enqueue_pos. */
        pos = m_enqueue_pos.load(std::memory_order_relaxed);
      }
    }

    cell->m_data = data;

    /* Increment the sequence so that the tail knows it's accessible */

    /* cell->m_pos 自增 1. */
    cell->m_pos.store(pos + 1, std::memory_order_release);

    return (true);
  }

  /** Dequeue an element
  @param[out]	data		Element read from the queue
  @return true on success */
  /* 元素出列. */
  bool dequeue(T &data) MY_ATTRIBUTE((warn_unused_result)) {
    Cell *cell;
    /* 获取当前的 m_dequeue_pos. */
    size_t pos = m_dequeue_pos.load(std::memory_order_relaxed);

    for (;;) {
      /* 以 m_capacity 取模求对应位置的元素. */
      cell = &m_ring[pos & m_capacity];

      /* 获取元素 cell 的 m_pos. */
      size_t seq = cell->m_pos.load(std::memory_order_acquire);

      /* 计算 cell->m_pos 和 m_dequeue_pos + 1 的差值. */
      auto diff = (intptr_t)seq - (intptr_t)(pos + 1);

      if (diff == 0) {
        /* Claim our spot by moving the head. If head isn't the same as we last
        checked then that means someone beat us to the punch. Weak compare is
        faster, but can return spurious results. Which in this instance is
        OK, because it's in the loop. */

        /* cell->m_pos 和 m_dequeue_pos + 1 相等代表 cell 已经是成功入列的元素,
         * 因为在每次成功入列后, cell->m_pos 会自增 1.
         * 尝试 m_dequeue_pos 自增 1,
         * 假如自增失败即代表当前位置的 cell 已被出列, 需要重新获取 m_dequeue_pos. */
        if (m_dequeue_pos.compare_exchange_weak(pos, pos + 1,
                                                std::memory_order_relaxed)) {
          break;
        }

      } else if (diff < 0) {
        /* The queue is empty */

        /* cell 入列成功会将 m_pos 自增 1, 所以假如 m_pos 小于 m_dequeue_pos + 1,
         * 即代表 cell 元素暂未入列. */
        return (false);

      } else {
        /* Under normal circumstances this branch should never be taken. */

        /* 重新获取 m_dequeue_pos. */
        pos = m_dequeue_pos.load(std::memory_order_relaxed);
      }
    }

    /* 获取对应元素的数据成员. */
    data = cell->m_data;

    /* Set the sequence to what the head sequence should be next
    time around */

    /* 更新 cell->m_pos 为  m_dequeue_pos + m_capacity + 1. */
    cell->m_pos.store(pos + m_capacity + 1, std::memory_order_release);

    return (true);
  }

  /** @return the capacity of the queue */
  size_t capacity() const MY_ATTRIBUTE((warn_unused_result)) {
    return (m_capacity + 1);
  }

  /** @return true if the queue is empty. */
  bool empty() const MY_ATTRIBUTE((warn_unused_result)) {
    size_t pos = m_dequeue_pos.load(std::memory_order_relaxed);

    for (;;) {
      auto cell = &m_ring[pos & m_capacity];

      size_t seq = cell->m_pos.load(std::memory_order_acquire);

      auto diff = (intptr_t)seq - (intptr_t)(pos + 1);

      if (diff == 0) {
        return (false);
      } else if (diff < 0) {
        return (true);
      } else {
        pos = m_dequeue_pos.load(std::memory_order_relaxed);
      }
    }

    return (false);
  }

 private:
  using Pad = byte[ut::INNODB_CACHE_LINE_SIZE];

  /* 队列的元素. */
  struct Cell {
    std::atomic<size_t> m_pos;
    T m_data;
  };

  using Aligned =
      typename std::aligned_storage<sizeof(Cell),
                                    std::alignment_of<Cell>::value>::type;

  Pad m_pad0;
  Cell *const m_ring;
  size_t const m_capacity;
  Pad m_pad1;
  std::atomic<size_t> m_enqueue_pos;
  Pad m_pad2;
  std::atomic<size_t> m_dequeue_pos;
  Pad m_pad3;

  mpmc_bq(mpmc_bq &&) = delete;
  mpmc_bq(const mpmc_bq &) = delete;
  mpmc_bq &operator=(mpmc_bq &&) = delete;
  mpmc_bq &operator=(const mpmc_bq &) = delete;
};

#endif /* ut0mpmcbq_h */
