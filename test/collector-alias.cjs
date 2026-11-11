const assert = require('node:assert/strict')
const { test } = require('node:test')
const mqtt = require('../')

// Independent hand-written wire bytes (not produced by the library encoder):
//   1) PUBLISH topic "a", topicAlias 1, payload "1"
//   2) PUBLISH topic "" (alias only), topicAlias 1, payload "2"
const wire = Buffer.from('30080001610323000131300700000323000132', 'hex')
const bindA = wire.subarray(0, 10)
const aliasOnly2 = wire.subarray(10)

// More hand-written fixtures.
const rebindB = Buffer.from('30080001620323000133', 'hex') // topic "b", alias 1, payload "3"
const aliasOnly4 = Buffer.from('300700000323000134', 'hex') // alias 1 only, payload "4"
const aliasZero = Buffer.from('30080001610323000035', 'hex') // topic "a", alias 0, payload "5"
const aliasZeroOnly = Buffer.from('300700000323000038', 'hex') // alias 0 only, payload "8"
const aliasTwoOnly = Buffer.from('300700000323000236', 'hex') // alias 2 only, payload "6"
const aliasThreeOnly = Buffer.from('300700000323000337', 'hex') // alias 3 only, payload "7"
const noTopicNoAlias = Buffer.from('300400000037', 'hex') // empty topic, no properties, payload "7"
const overMaxBind = Buffer.from('30080001630323000239', 'hex') // topic "c", alias 2, payload "9"

function collector (opts) {
  const parser = mqtt.parser(opts)
  const packets = []
  const errors = []
  parser.on('packet', p => packets.push(p))
  parser.on('error', e => errors.push(e))
  return { parser, packets, errors }
}

function summary (packets) {
  return packets.map(p => ({
    cmd: p.cmd,
    topic: p.topic,
    payload: p.payload == null ? p.payload : p.payload.toString(),
    qos: p.qos,
    messageId: p.messageId,
    alias: p.properties ? p.properties.topicAlias : undefined
  }))
}

function feed (parser, chunks) {
  for (const chunk of chunks) parser.parse(chunk)
}

test('alias mode: construction requires MQTT 5 and an integer 0..65535 maximum', () => {
  assert.throws(() => mqtt.parser({ resolveTopicAliases: true }), TypeError)
  assert.throws(() => mqtt.parser({ resolveTopicAliases: true, protocolVersion: 4 }), TypeError)
  assert.throws(() => mqtt.parser({ resolveTopicAliases: true, protocolVersion: 3 }), TypeError)
  for (const bad of [-1, 65536, 1.5, NaN, '10', null]) {
    assert.throws(
      () => mqtt.parser({ resolveTopicAliases: true, protocolVersion: 5, topicAliasMaximum: bad }),
      TypeError
    )
  }
  // Default maximum is 0 and the boundary values are accepted.
  for (const maximum of [undefined, 0, 1, 65535]) {
    const parser = mqtt.parser({ resolveTopicAliases: true, protocolVersion: 5, topicAliasMaximum: maximum })
    assert.equal(typeof parser.resetConnection, 'function')
  }
  // The flag is opt-in: plain parsers keep the original raw behaviour.
  assert.doesNotThrow(() => mqtt.parser({ protocolVersion: 4 }))
})

test('alias mode: canonical fixture resolves both packets to topic a, default mode keeps empty topic', () => {
  const enabled = collector({ resolveTopicAliases: true, protocolVersion: 5, topicAliasMaximum: 10 })
  feed(enabled.parser, [wire])
  assert.deepEqual(summary(enabled.packets), [
    { cmd: 'publish', topic: 'a', payload: '1', qos: 0, messageId: undefined, alias: 1 },
    { cmd: 'publish', topic: 'a', payload: '2', qos: 0, messageId: undefined, alias: 1 }
  ])
  assert.deepEqual(enabled.errors, [])

  const raw = collector({ protocolVersion: 5 })
  feed(raw.parser, [wire])
  assert.equal(raw.packets[0].topic, 'a')
  assert.equal(raw.packets[1].topic, '')
})

test('alias mode: whole stream, every binary split and byte-at-a-time give the same sequence', () => {
  const qos1 = mqtt.generate({
    cmd: 'publish', qos: 1, messageId: 7, topic: 'b', payload: 'z', properties: { topicAlias: 2 }
  }, { protocolVersion: 5 })
  // The public generator must agree with the independent hand-written bytes.
  assert.equal(qos1.toString('hex'), '320a0001620007032300027a')
  assert.equal(
    mqtt.generate({ cmd: 'publish', qos: 0, topic: '', payload: '2', properties: { topicAlias: 1 } },
      { protocolVersion: 5 }).toString('hex'),
    aliasOnly2.toString('hex')
  )
  assert.equal(
    mqtt.generate({ cmd: 'publish', qos: 0, topic: 'b', payload: '3', properties: { topicAlias: 1 } },
      { protocolVersion: 5 }).toString('hex'),
    rebindB.toString('hex')
  )

  const opts = { resolveTopicAliases: true, protocolVersion: 5, topicAliasMaximum: 10 }
  const expected = [
    { cmd: 'publish', topic: 'a', payload: '1', qos: 0, messageId: undefined, alias: 1 },
    { cmd: 'publish', topic: 'a', payload: '2', qos: 0, messageId: undefined, alias: 1 },
    { cmd: 'publish', topic: 'b', payload: 'z', qos: 1, messageId: 7, alias: 2 },
    { cmd: 'publish', topic: 'b', payload: '3', qos: 0, messageId: undefined, alias: 1 },
    { cmd: 'publish', topic: 'b', payload: '4', qos: 0, messageId: undefined, alias: 1 }
  ]

  for (const stream of [wire, Buffer.concat([wire, qos1, rebindB, aliasOnly4])]) {
    const whole = collector(opts)
    feed(whole.parser, [stream])
    assert.deepEqual(summary(whole.packets), expected.slice(0, summary(whole.packets).length))
    const ref = summary(whole.packets)

    for (let i = 0; i <= stream.length; i++) {
      const split = collector(opts)
      feed(split.parser, [stream.subarray(0, i), stream.subarray(i)])
      assert.deepEqual(summary(split.packets), ref)
      assert.deepEqual(split.errors, [])
    }

    const bytewise = collector(opts)
    feed(bytewise.parser, [...stream].map(b => Buffer.from([b])))
    assert.deepEqual(summary(bytewise.packets), ref)
  }
})

test('alias mode: topic without alias passes through and never touches the mapping', () => {
  const c = collector({ resolveTopicAliases: true, protocolVersion: 5, topicAliasMaximum: 10 })
  feed(c.parser, [
    bindA,
    mqtt.generate({ cmd: 'publish', qos: 0, topic: 'plain', payload: 'p' }, { protocolVersion: 5 }),
    aliasOnly2
  ])
  assert.deepEqual(summary(c.packets).map(p => p.topic), ['a', 'plain', 'a'])
  assert.equal(c.packets[1].properties, undefined)
})

test('alias mode: a later binding overrides the earlier one', () => {
  const c = collector({ resolveTopicAliases: true, protocolVersion: 5, topicAliasMaximum: 10 })
  feed(c.parser, [bindA, aliasOnly2, rebindB, aliasOnly4])
  assert.deepEqual(summary(c.packets).map(p => p.topic), ['a', 'a', 'b', 'b'])
})

test('alias mode: payload, QoS, messageId and the original properties are preserved', () => {
  const c = collector({ resolveTopicAliases: true, protocolVersion: 5, topicAliasMaximum: 10 })
  const qos1 = mqtt.generate({
    cmd: 'publish', qos: 1, messageId: 42, dup: true, retain: true,
    topic: 'b', payload: Buffer.from('z'), properties: { topicAlias: 2 }
  }, { protocolVersion: 5 })
  feed(c.parser, [bindA, qos1, aliasOnly2])
  const resolved = c.packets[2]
  assert.equal(resolved.topic, 'a')
  assert.deepEqual(resolved.payload, Buffer.from('2'))
  assert.equal(resolved.qos, 0)
  assert.equal(resolved.messageId, undefined)
  assert.deepEqual(resolved.properties, { topicAlias: 1 })
  assert.equal(c.packets[1].messageId, 42)
  assert.equal(c.packets[1].qos, 1)
  assert.equal(c.packets[1].dup, true)
  assert.equal(c.packets[1].retain, true)

  // Non-PUBLISH packets still pass through untouched.
  const ping = collector({ resolveTopicAliases: true, protocolVersion: 5, topicAliasMaximum: 10 })
  ping.parser.parse(mqtt.generate({ cmd: 'pingreq' }, { protocolVersion: 5 }))
  assert.equal(ping.packets[0].cmd, 'pingreq')
})

test('alias mode: mutating an emitted packet cannot corrupt the alias table', () => {
  const c = collector({ resolveTopicAliases: true, protocolVersion: 5, topicAliasMaximum: 10 })
  feed(c.parser, [bindA])
  c.packets[0].topic = 'hacked'
  c.packets[0].properties.topicAlias = 999
  feed(c.parser, [aliasOnly2])
  assert.equal(c.packets[1].topic, 'a')
  assert.equal(c.packets[1].properties.topicAlias, 1)
})

test('alias mode: default maximum 0 forbids every alias but plain topics pass through', () => {
  const c = collector({ resolveTopicAliases: true, protocolVersion: 5 })
  c.parser.parse(bindA)
  assert.equal(c.packets.length, 0)
  assert.equal(c.errors.length, 1)

  const plain = collector({ resolveTopicAliases: true, protocolVersion: 5 })
  plain.parser.parse(mqtt.generate({ cmd: 'publish', qos: 0, topic: 'a', payload: '1' },
    { protocolVersion: 5 }))
  assert.equal(plain.packets.length, 1)
  assert.equal(plain.packets[0].topic, 'a')
  assert.equal(plain.errors.length, 0)
})

test('alias mode: QoS 1 alias-only packet keeps its messageId after resolution', () => {
  const c = collector({ resolveTopicAliases: true, protocolVersion: 5, topicAliasMaximum: 10 })
  const qos1Bind = mqtt.generate({
    cmd: 'publish', qos: 1, messageId: 9, topic: 'a', payload: '1', properties: { topicAlias: 1 }
  }, { protocolVersion: 5 })
  const qos1AliasOnly = mqtt.generate({
    cmd: 'publish', qos: 1, messageId: 10, topic: '', payload: '2', properties: { topicAlias: 1 }
  }, { protocolVersion: 5 })
  assert.equal(qos1AliasOnly.toString('hex'), '32090000000a0323000132')
  c.parser.parse(Buffer.concat([qos1Bind, qos1AliasOnly]))
  assert.equal(c.packets.length, 2)
  assert.equal(c.packets[1].topic, 'a')
  assert.equal(c.packets[1].messageId, 10)
  assert.equal(c.packets[1].qos, 1)
  assert.deepEqual(c.packets[1].payload, Buffer.from('2'))
  assert.equal(c.errors.length, 0)
})

test('alias mode: the illegal PUBLISH shapes emit one error and no packet', () => {
  const cases = [
    ['alias 0 with a topic', aliasZero, 1],
    ['alias 0 without a topic', aliasZeroOnly, 1],
    ['alias above the negotiated maximum', aliasTwoOnly, 1],
    ['unmapped alias-only packet', aliasThreeOnly, 10],
    ['neither topic nor alias', noTopicNoAlias, 10]
  ]
  for (const [name, buf, maximum] of cases) {
    const c = collector({ resolveTopicAliases: true, protocolVersion: 5, topicAliasMaximum: maximum })
    c.parser.parse(buf)
    assert.equal(c.packets.length, 0, name + ' must not emit a packet')
    assert.equal(c.errors.length, 1, name + ' must emit an error')
    assert.ok(c.errors[0] instanceof Error, name)
  }
})

test('alias mode: a failed binding commits no mapping', () => {
  // alias 2 exceeds a maximum of 1: the binding is rejected.
  const c = collector({ resolveTopicAliases: true, protocolVersion: 5, topicAliasMaximum: 1 })
  c.parser.parse(overMaxBind)
  assert.equal(c.packets.length, 0)
  assert.equal(c.errors.length, 1)
  c.parser.resetConnection()
  // After reset the rejected mapping is gone: the alias must be re-bound first.
  c.parser.parse(aliasTwoOnly)
  assert.equal(c.packets.length, 0)
  assert.equal(c.errors.length, 2)
})

test('alias mode: trailing and future input stay silent after an alias error', () => {
  const c = collector({ resolveTopicAliases: true, protocolVersion: 5, topicAliasMaximum: 10 })
  // Good packet, illegal packet, another good packet in one parse() call.
  c.parser.parse(Buffer.concat([bindA, aliasThreeOnly, aliasOnly2]))
  assert.deepEqual(summary(c.packets).map(p => p.topic), ['a'])
  assert.equal(c.errors.length, 1)

  // Later parse() calls stay silent, including data arriving byte by byte.
  for (const b of aliasOnly2) c.parser.parse(Buffer.from([b]))
  assert.equal(c.packets.length, 1)
  assert.equal(c.errors.length, 1)

  // A split illegal frame emits exactly one error as well.
  const split = collector({ resolveTopicAliases: true, protocolVersion: 5, topicAliasMaximum: 10 })
  split.parser.parse(aliasThreeOnly.subarray(0, 4))
  split.parser.parse(aliasThreeOnly.subarray(4))
  assert.equal(split.packets.length, 0)
  assert.equal(split.errors.length, 1)
})

test('alias mode: resetConnection clears half frames and mappings but keeps listeners and config', () => {
  const c = collector({ resolveTopicAliases: true, protocolVersion: 5, topicAliasMaximum: 10 })
  c.parser.parse(bindA)
  // Half a packet arrives, then the connection is replaced before it completes.
  c.parser.parse(aliasOnly2.subarray(0, 4))
  c.parser.resetConnection()

  // Old mappings are gone: an alias-only packet must now fail.
  c.parser.parse(aliasOnly2)
  assert.equal(c.packets.length, 1) // the binding packet emitted before reset
  assert.equal(c.packets[0].topic, 'a')
  assert.equal(c.errors.length, 1)

  // Configuration survives: alias 11 still exceeds the negotiated maximum.
  c.parser.resetConnection()
  c.parser.parse(Buffer.from('300700000323000b31', 'hex'))
  assert.equal(c.errors.length, 2)

  // Listeners survive: a fresh full binding re-enables the alias.
  c.parser.resetConnection()
  c.parser.parse(bindA)
  c.parser.parse(aliasOnly2)
  assert.equal(c.packets.length, 3)
  assert.equal(c.packets[2].topic, 'a')
  assert.equal(c.errors.length, 2)

  // The half frame from before the first reset never leaked into the stream.
  const clean = collector({ resolveTopicAliases: true, protocolVersion: 5, topicAliasMaximum: 10 })
  clean.parser.parse(aliasOnly2.subarray(0, 4))
  clean.parser.resetConnection()
  clean.parser.parse(bindA)
  assert.equal(clean.packets.length, 1)
  assert.equal(clean.packets[0].payload.toString(), '1')
})

test('default mode: the existing parse()-after-error recovery is unchanged', () => {
  const c = collector({ protocolVersion: 4 })
  c.parser.parse(Buffer.from('3700', 'hex')) // both QoS bits set
  assert.equal(c.errors.length, 1)
  c.parser.parse(Buffer.from('300400016178', 'hex')) // valid v4 PUBLISH "a"/"x"
  assert.equal(c.packets.length, 1)
  assert.equal(c.packets[0].topic, 'a')
  assert.equal(c.packets[0].payload.toString(), 'x')
})
