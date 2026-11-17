(module
  (type $cb (func))
  (type $relay_type (func (param (ref null $cb)) (result (ref null $cb))))
  (import "env" "memory" (memory 1))
  (import "env" "opaque" (func $opaque (type $relay_type)))
  (func $a (type $cb))
  (func $b (type $cb))
  (elem declare func $a $b)
  (func $relay (type $relay_type) local.get 0)
  (func $left ref.func $a call $relay call_ref $cb)
  (func $right ref.func $b call $relay call_ref $cb)
  (func $locals (local $p (ref null $cb))
    ref.func $a local.set $p
    ref.func $b local.set $p
    local.get $p call_ref $cb)
  (func $join (local $p (ref null $cb))
    i32.const 0
    if
      ref.func $a local.set $p
    else
      ref.func $b local.set $p
    end
    local.get $p call_ref $cb)
  (func $bridge (type $relay_type)
    i32.const 0
    if (result (ref null $cb))
      local.get 0
    else
      local.get 0 call $peer
    end)
  (func $peer (type $relay_type) local.get 0 call $bridge)
  (func $recursive_root ref.func $a call $peer call_ref $cb)
  (func $spin (result (ref null $cb)) call $spin)
  (func $spin_root call $spin call_ref $cb ref.func $b call_ref $cb)
  (func $null_root ref.null $cb call_ref $cb ref.func $b call_ref $cb)
  (func $external_root ref.null $cb call $opaque call_ref $cb)
  (func $tail (type $relay_type) local.get 0 return_call $relay)
  (export "left" (func $left))
  (export "right" (func $right))
)
