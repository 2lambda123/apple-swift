// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend-emit-module -emit-module-path %t/FakeDistributedActorSystems.swiftmodule -module-name FakeDistributedActorSystems -disable-availability-checking %S/../Inputs/FakeDistributedActorSystems.swift
// RUN: %target-build-swift -module-name main -Xfrontend -enable-experimental-distributed -Xfrontend -disable-availability-checking -j2 -parse-as-library -I %t %s %S/../Inputs/FakeDistributedActorSystems.swift -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: %target-run %t/a.out | %FileCheck %s --color

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: distributed

// rdar://76038845
// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

// FIXME(distributed): Distributed actors currently have some issues on windows rdar://82593574
// UNSUPPORTED: OS=windows-msvc


import Distributed
import FakeDistributedActorSystems

typealias DefaultDistributedActorSystem = FakeRoundtripActorSystem

distributed actor Worker {
//  nonisolated var unownedExecutor: UnownedSerialExecutor {
//    print("get unowned executor")
//    return MainActor.sharedUnownedExecutor
//  }

  distributed func test(x: Int) async throws {
    print("executed: \(#function)")
    assumeOnMainActorExecutor {
      print("assume: this distributed actor shares executor with MainActor")
    }
    print("done executed: \(#function)")
  }

}

@main struct Main {
  static func main() async {
    let worker = Worker(actorSystem: DefaultDistributedActorSystem())
    // CHECK: | assign id
    // CHECK: | actor ready

    try! await worker.test(x: 42)
    // CHECK: get unowned executor
    // CHECK: executed: test(x:)
    // CHECK: assume: this distributed actor shares executor with MainActor
    // CHECK: done executed: test(x:)

    print("OK") // CHECK: OK
  }
}