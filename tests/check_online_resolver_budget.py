#!/usr/bin/env python3
"""Bind both native resolver clients to shared work/ownership limits; no network."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


def section(source, start, end):
    return source.split(start, 1)[1].split(end, 1)[0]


class ResolverBudgetContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.clients = {
            name: (ROOT / "platform/online" / name).read_text()
            for name in ("match_live_transport.cpp", "match_signal_client.cpp")
        }
        cls.budget = (ROOT / "platform/online/async_work_budget.h").read_text()

    def test_one_external_inline_process_pool(self):
        self.assertRegex(self.budget, r"(?m)^inline AsyncWorkBudget &onlineResolverWorkBudget\(\)")
        accessor = section(self.budget, "inline AsyncWorkBudget &onlineResolverWorkBudget()", "#endif")
        self.assertIn("static mdkr_async_work_detail::ResolverBudgetOwner owner(slot);", accessor)
        self.assertIn("return owner.budget();", accessor)
        self.assertIn(": budget_(8), slot_(slot)", self.budget)
        self.assertNotRegex(self.budget, r"namespace\s*\{")

    def test_cleanup_observation_never_initializes_the_pool(self):
        observer = section(self.budget, "inline std::size_t onlineResolverWorkInUse() noexcept", "#endif")
        self.assertNotIn("onlineResolverWorkBudget()", observer)
        self.assertNotRegex(observer, r"make_shared|\bnew\b|ResolverBudgetOwner")
        self.assertIn("return budget != nullptr ? budget->inUse() : 0u;", observer)
        self.assertIn("std::memory_order_acquire", observer)
        owner = section(self.budget, "class ResolverBudgetOwner {", "} // namespace")
        self.assertIn("slot_.store(&budget_, std::memory_order_release);", owner)
        destructor = section(owner, "~ResolverBudgetOwner() {", "\n    }")
        self.assertIn("slot_.store(nullptr, std::memory_order_release);", destructor)
        accessor = section(self.budget, "inline AsyncWorkBudget &onlineResolverWorkBudget()", "\n}")
        self.assertLess(accessor.index("resolverBudgetSlot()"), accessor.index("static mdkr_async_work_detail"))

    def test_both_clients_acquire_before_allocating_or_starting_lookup(self):
        for name, source in self.clients.items():
            with self.subTest(client=name):
                self.assertIn('#include "async_work_budget.h"', source)
                resolve = section(source, "bool resolveAddresses(", "\n}\n")
                acquire = resolve.index("onlineResolverWorkBudget().tryAcquire()")
                for allocation in ("std::string ownedHost", "std::make_shared<ResolveTask>",
                                   "helper = std::thread("):
                    self.assertLess(acquire, resolve.index(allocation))
                capacity_wait = resolve[:resolve.index("std::string ownedHost")]
                self.assertIn("now >= deadlineMs", capacity_wait)
                self.assertTrue("abort->load()" in capacity_wait or "stopping" in capacity_wait)
                self.assertRegex(capacity_wait, r"if\s*\(permit\)\s*break;")
                self.assertIn("remaining < kPollSliceMs ? remaining : kPollSliceMs", capacity_wait)
                self.assertIn("std::make_shared<ResolveTask>(std::move(permit), network)", resolve)

    def test_permit_outlives_task_results_and_synchronization(self):
        for name, source in self.clients.items():
            with self.subTest(client=name):
                task = section(source, "struct ResolveTask {", "\n};")
                permit = task.index("AsyncWorkBudget::Permit permit;")
                self.assertLess(permit, task.index("std::mutex mutex;"))
                self.assertLess(permit, task.index("std::condition_variable cv;"))
                self.assertLess(permit, task.index("AddrinfoOwner results{nullptr, ::freeaddrinfo};"))
                self.assertIn("permit(std::move(ownedPermit))", task)
                resolve = section(source, "bool resolveAddresses(", "\n}\n")
                self.assertNotIn("task->permit =", resolve)
                self.assertNotIn("task->permit.release", resolve)
                self.assertNotIn("task->permit.reset", resolve)

    def test_workers_capture_owned_values_and_check_abandonment_before_lookup(self):
        for name, source in self.clients.items():
            with self.subTest(client=name):
                resolve = section(source, "bool resolveAddresses(", "\n}\n")
                worker = re.search(r"helper = std::thread\(\s*\[([^\]]*)\]\(\) \{(.*?)\n\s*\}\);",
                                   resolve, re.S)
                self.assertIsNotNone(worker)
                captures, body = worker.groups()
                self.assertNotIn("&", captures)
                self.assertNotIn("this", captures)
                self.assertIn("task", captures)
                self.assertIn("host = std::move(ownedHost)", captures)
                self.assertIn("port = std::move(ownedPort)", captures)
                self.assertLess(body.index("if (task->abandoned)"), body.index("::getaddrinfo("))
                self.assertIn("AddrinfoOwner results(rawResults, ::freeaddrinfo);", body)
                self.assertIn("if (!task->abandoned)", body)
                self.assertIn("task->results = std::move(results);", body)
                self.assertNotIn("std::to_string(", body)

    def test_waiter_owns_results_and_failures_do_not_escape_transport_worker(self):
        for name, source in self.clients.items():
            with self.subTest(client=name):
                resolve = section(source, "bool resolveAddresses(", "\n}\n")
                task = resolve.index("auto task = std::make_shared<ResolveTask>")
                waiter_results = resolve.index("AddrinfoOwner results(nullptr, ::freeaddrinfo);")
                self.assertLess(task, waiter_results)
                self.assertIn("results = std::move(task->results);", resolve)
                self.assertIn("appendAddrinfo(results.get(), out);", resolve)
                self.assertIn("while (!task->done)", resolve)
                self.assertIn("task->abandoned = true;", resolve)
                self.assertRegex(resolve, r"catch \(\.\.\.\) \{[^{}]*out\.clear\(\);\s*return false;\s*\}\s*$")

    def test_detach_failure_preserves_thread_ownership(self):
        for name, source in self.clients.items():
            with self.subTest(client=name):
                resolve = section(source, "bool resolveAddresses(", "\n}\n")
                failure = section(resolve, "helper.detach();", "AddrinfoOwner results(nullptr")
                self.assertIn("catch (...)", failure)
                self.assertIn("if (helper.joinable())", failure)
                self.assertLess(failure.index("task->abandoned = true;"), failure.index("helper.join();"))


if __name__ == "__main__":
    unittest.main()
