// RUN: %target-swift-emit-silgen -enable-experimental-feature MoveOnlyEnumDeinits -module-name test %s | %FileCheck %s --enable-var-scope
// RUN: %target-swift-emit-sil -enable-experimental-feature MoveOnlyEnumDeinits -module-name test -sil-verify-all %s | %FileCheck %s --check-prefix CHECK-SIL --enable-var-scope

func invokedDeinit() {}

@_moveOnly enum MaybeFile {
  case some(File)
  case none

  deinit {}

  // NOTE: we can't pattern match on self since
  // that would consume it before we can forget self!
  var test: Int {
    __consuming get {
      _forget self
      return 0
    }
  }

  // CHECK-LABEL: sil hidden [ossa] @$s4test9MaybeFileOAASivg
  // CHECK:    [[SELF_BOX:%.*]] = alloc_box ${ let MaybeFile }, let, name "self", argno 1
  // CHECK:    [[SELF_REF:%.*]] = project_box [[SELF_BOX]] : ${ let MaybeFile }, 0
  // CHECK:    store {{.*}} to [init]
  // CHECK:    [[SELF_MMC:%.*]] = mark_must_check [no_consume_or_assign] [[SELF_REF]] : $*MaybeFile
  // CHECK:    [[SELF_VAL:%.*]] = load [copy] [[SELF_MMC]] : $*MaybeFile
  // CHECK:    switch_enum [[SELF_VAL]] : $MaybeFile, case #MaybeFile.some!enumelt: bb1, case #MaybeFile.none!enumelt: bb2
  //
  // CHECK:  bb1([[FILE:%.*]] : @owned $File):
  // CHECK:    destroy_value [[FILE]] : $File
}

@_moveOnly struct File {
  let fd: Int
  static var nextFD: Int = 0

  init() {
    fd = File.nextFD
    File.nextFD += 1
  }

  __consuming func takeDescriptor() -> Int {
    let id = fd
    _forget self
    return id
  }

  // CHECK-LABEL: sil hidden [ossa] @$s4test4FileV14takeDescriptorSiyF
  // CHECK:  [[SELF_BOX:%.*]] = alloc_box ${ let File }, let, name "self", argno 1
  // CHECK:  [[SELF_REF:%.*]] = project_box [[SELF_BOX]] : ${ let File }, 0
  // CHECK:  store {{.*}} to [init]
  // CHECK:  load_borrow {{.*}} : $*File
  // CHECK:  [[SELF_MMC:%.*]] = mark_must_check [no_consume_or_assign] [[SELF_REF]] : $*File
  // CHECK:  [[SELF_VAL:%.*]] = load [copy] [[SELF_MMC]] : $*File
  // CHECK:  end_lifetime [[SELF_VAL]] : $File

  deinit {
    invokedDeinit()
  }
}

@_moveOnly struct PointerTree {
  let left: Ptr = Ptr()
  let file: File = File()
  let popularity: Int = 0
  var right: Ptr = Ptr()

  consuming func tryDestroy(doForget: Bool) throws {
    if doForget {
      _forget self
    }
    throw E.err
  }

// CHECK-LABEL: sil hidden [ossa] @$s4test11PointerTreeV10tryDestroy8doForgetySb_tKF : $@convention(method) (Bool, @owned PointerTree) -> @error any Error {
// CHECK:   bb0{{.*}}:
// CHECK:     [[SELF_BOX:%.*]] = alloc_box ${ var PointerTree }, var, name "self"
// CHECK:     [[SELF:%.*]] = begin_borrow [lexical] [[SELF_BOX]] : ${ var PointerTree }
// CHECK:     [[SELF_PTR:%.*]] = project_box [[SELF]] : ${ var PointerTree }, 0
//            .. skip to the conditional test ..
// CHECK:     [[SHOULD_THROW:%.*]] = struct_extract {{.*}} : $Bool, #Bool._value
// CHECK:     cond_br [[SHOULD_THROW]], bb1, bb2
//
// CHECK:   bb1:
// CHECK:     [[ACCESS:%.*]] = begin_access [read] [unknown] [[SELF_PTR]] : $*PointerTree
// CHECK:     [[MMC:%.*]] = mark_must_check [no_consume_or_assign] [[ACCESS]] : $*PointerTree
// CHECK:     [[COPIED_SELF:%.*]] = load [copy] [[MMC]] : $*PointerTree
// CHECK:     end_access [[ACCESS]] : $*PointerTree
// CHECK:     ([[LEFT:%.*]], [[FILE:%.*]], {{%.*}}, [[RIGHT:%.*]]) = destructure_struct [[COPIED_SELF]] : $PointerTree
// CHECK:     destroy_value [[LEFT]] : $Ptr
// CHECK:     destroy_value [[FILE]] : $File
// CHECK:     destroy_value [[RIGHT]] : $Ptr
// CHECK:     br bb3
//
// CHECK:   bb2:
// CHECK:     br bb3
//
// CHECK:   bb3:
// CHECK:     end_borrow [[SELF]] : ${ var PointerTree }
// CHECK:     destroy_value [[SELF_BOX]] : ${ var PointerTree }
// CHECK:     throw
// CHECK: } // end sil function

// After the mandatory passes have run, check for correct deinitializations within the init.

// CHECK-SIL-LABEL: sil hidden @$s4test11PointerTreeV10tryDestroy8doForgetySb_tKF
// CHECK-SIL:     [[SHOULD_THROW:%.*]] = struct_extract {{.*}} : $Bool, #Bool._value
// CHECK-SIL:     cond_br [[SHOULD_THROW]], bb1, bb2
//
// CHECK-SIL:  bb1:
// CHECK-SIL:     [[ACCESS:%.*]] = begin_access [modify] [static] {{.*}} : $*PointerTree
// CHECK-SIL:     [[SELF_VAL:%.*]] = load [[ACCESS]] : $*PointerTree
// CHECK-SIL:     end_access [[ACCESS]] : $*PointerTree
// CHECK-SIL:     [[LEFT:%.*]] = struct_extract [[SELF_VAL]] : $PointerTree, #PointerTree.left
// CHECK-SIL:     [[FILE:%.*]] = struct_extract [[SELF_VAL]] : $PointerTree, #PointerTree.file
// CHECK-SIL:     [[RIGHT:%.*]] = struct_extract [[SELF_VAL]] : $PointerTree, #PointerTree.right
// CHECK-SIL:     strong_release [[LEFT]] : $Ptr
// CHECK-SIL:     [[FILE_DEINIT:%.*]] = function_ref @$s4test4FileVfD : $@convention(method) (@owned File) -> ()
// CHECK-SIL:     apply [[FILE_DEINIT]]([[FILE]])
// CHECK-SIL:     strong_release [[RIGHT]] : $Ptr
// CHECK-SIL:     br bb3
//
// CHECK-SIL:  bb2:
// CHECK-SIL:     [[TREE_DEINIT:%.*]] = function_ref @$s4test11PointerTreeVfD : $@convention(method) (@owned PointerTree) -> ()
// CHECK-SIL:     [[SELF_VAL:%.*]] = load {{.*}} : $*PointerTree
// CHECK-SIL:     apply [[TREE_DEINIT]]([[SELF_VAL]]) : $@convention(method) (@owned PointerTree) -> ()
// CHECK-SIL:     br bb3
//
// CHECK-SIL:  bb3:
// CHECK-SIL-NOT:  apply
// CHECK-SIL:      throw


  deinit {
    invokedDeinit()
  }
}

final class Wallet {
  var numCards = 0
}

@_moveOnly enum Ticket {
  case empty
  case within(Wallet)

  consuming func changeTicket(inWallet wallet: Wallet? = nil) {
    if let existingWallet = wallet {
      _forget self
      self = .within(existingWallet)
    }
  }
  // As of now, we allow reinitialization after forget. Not sure if this is intended.
  // CHECK-LABEL: sil hidden [ossa] @$s4test6TicketO06changeB08inWalletyAA0E0CSg_tF : $@convention(method) (@guaranteed Optional<Wallet>, @owned Ticket) -> () {
  // CHECK:    [[SELF_REF:%.*]] = project_box [[SELF_BOX:%.*]] : ${ var Ticket }, 0
  // CHECK:    switch_enum {{.*}} : $Optional<Wallet>, case #Optional.some!enumelt: [[HAVE_WALLET_BB:bb.*]], case #Optional.none!enumelt: {{.*}}
  //
  // >> now we begin the destruction sequence, which involves pattern matching on self to destroy its innards
  // CHECK:  [[HAVE_WALLET_BB]]({{%.*}} : @owned $Wallet):
  // CHECK:    [[SELF_ACCESS:%.*]] = begin_access [read] [unknown] {{%.*}} : $*Ticket
  // CHECK:    [[SELF_MMC:%.*]] = mark_must_check [no_consume_or_assign] [[SELF_ACCESS]]
  // CHECK:    [[SELF_COPY:%.*]] = load [copy] [[SELF_MMC]] : $*Ticket
  // CHECK:    end_access [[SELF_ACCESS:%.*]] : $*Ticket
  // CHECK:    switch_enum [[SELF_COPY]] : $Ticket, case #Ticket.empty!enumelt: [[TICKET_EMPTY:bb[0-9]+]], case #Ticket.within!enumelt: [[TICKET_WITHIN:bb[0-9]+]]
  // CHECK:  [[TICKET_EMPTY]]:
  // CHECK:    br [[JOIN_POINT:bb[0-9]+]]
  // CHECK:  [[TICKET_WITHIN]]([[PREV_SELF_WALLET:%.*]] : @owned $Wallet):
  // CHECK:    destroy_value [[PREV_SELF_WALLET]] : $Wallet
  // CHECK:    br [[JOIN_POINT]]
  // >> from here on we are reinitializing self.
  // CHECK:  [[JOIN_POINT]]:
  // CHECK:    [[NEW_SELF_VAL:%.*]] = enum $Ticket, #Ticket.within!enumelt, {{.*}} : $Wallet
  // CHECK:    [[SELF_ACCESS2:%.*]] = begin_access [modify] [unknown] [[SELF_REF]] : $*Ticket
  // CHECK:    [[SELF_MMC2:%.*]] = mark_must_check [assignable_but_not_consumable] [[SELF_ACCESS2]] : $*Ticket
  // CHECK:    assign [[NEW_SELF_VAL]] to [[SELF_MMC2]] : $*Ticket
  // CHECK:    end_access [[SELF_ACCESS2]] : $*Ticket

  deinit {
    print("destroying ticket")
  }
}

enum E: Error { case err }
class Ptr { var whatever: Int = 0 }
