/*
	Copyright 2023 Loophole Labs

	Licensed under the Apache License, Version 2.0 (the "License");
	you may not use this file except in compliance with the License.
	You may obtain a copy of the License at

		   http://www.apache.org/licenses/LICENSE-2.0

	Unless required by applicable law or agreed to in writing, software
	distributed under the License is distributed on an "AS IS" BASIS,
	WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
	See the License for the specific language governing permissions and
	limitations under the License.
*/

package expression

import (
	"encoding/binary"
	"fmt"
	"math"

	"github.com/loopholelabs/wasm-toolkit/pkg/wasm/encoding"
	"github.com/loopholelabs/wasm-toolkit/pkg/wasm/types"
)

// decodeBlockType decodes an empty block type (0x40), a single value type, or
// a signed LEB33 non-negative type index. The value type bytes are all
// negative LEB33 values, so reading a signed LEB33 distinguishes the three
// forms unambiguously.
func decodeBlockType(data []byte, ptr *int, expr *Expression) error {
	if *ptr >= len(data) {
		return fmt.Errorf("Truncated block type at pc %d", expr.PC)
	}
	val, n := encoding.DecodeSleb128(data[*ptr:])
	if n <= 0 {
		return fmt.Errorf("Invalid block type at pc %d", expr.PC)
	}
	*ptr += n
	switch {
	case val == -64: // 0x40, empty block type
		expr.Result = types.ValNone
	case val >= 0:
		// Non-negative signed LEB33 value: a type index.
		if val > math.MaxUint32 {
			return fmt.Errorf("Block type index %d out of range at pc %d", val, expr.PC)
		}
		expr.BlockTypeIsIndex = true
		expr.BlockTypeIndex = int(val)
	case val >= -32:
		// Single value type, encoded as a negative signed LEB33 value:
		// -1 = 0x7f (i32) through -17 = 0x6f (externref), and exnref 0x69.
		expr.Result = types.ValType(byte(val & 0x7f))
	default:
		return fmt.Errorf("Unsupported block type %d at pc %d", val, expr.PC)
	}
	return nil
}

func NewExpression(data []byte, pc uint64) ([]*Expression, int, error) {

	// This determines when we are finished
	nestCounter := 1

	exps := make([]*Expression, 0)
	ptr := 0

	for {
		if ptr == len(data) {
			break // All done
		}
		opptr := ptr
		opcode := data[ptr]
		ptr++

		expr := &Expression{
			PC:     pc + uint64(opptr),
			Opcode: Opcode(opcode),
		}

		// First deal with simple opcodes (No args)
		if expr.HasNoArgs() && expr.Opcode != InstrToOpcode["end"] {
			// Fall through and add it as it is.
		} else if Opcode(opcode) == InstrToOpcode["br_table"] {
			numLabels, l := binary.Uvarint(data[ptr:])
			ptr += l
			labels := make([]int, 0)
			for ll := 0; ll < int(numLabels); ll++ {
				labelIdx, l := binary.Uvarint(data[ptr:])
				ptr += l
				labels = append(labels, int(labelIdx))
			}
			defaultLabelIdx, l := binary.Uvarint(data[ptr:])
			ptr += l
			expr.Labels = labels
			expr.LabelIndex = int(defaultLabelIdx)
		} else if Opcode(opcode) == InstrToOpcode["br"] ||
			Opcode(opcode) == InstrToOpcode["br_if"] {
			val, l := binary.Uvarint(data[ptr:])
			ptr += l
			expr.LabelIndex = int(val)
		} else if expr.HasMemoryArgs() {
			align, l := binary.Uvarint(data[ptr:])
			ptr += l
			offset, l := binary.Uvarint(data[ptr:])
			ptr += l
			expr.MemAlign = int(align)
			expr.MemOffset = int(offset)
		} else if Opcode(opcode) == InstrToOpcode["memory.size"] ||
			Opcode(opcode) == InstrToOpcode["memory.grow"] {
			//				memoryIndex := data[ptr]
			// TODO: Use this to support multiple memories etc
			ptr++
		} else if Opcode(opcode) == InstrToOpcode["block"] ||
			Opcode(opcode) == InstrToOpcode["if"] ||
			Opcode(opcode) == InstrToOpcode["loop"] {
			// Read the blocktype
			if err := decodeBlockType(data, &ptr, expr); err != nil {
				return nil, 0, err
			}
			nestCounter++
		} else if Opcode(opcode) == InstrToOpcode["try_table"] {
			// The block type is followed by a catch vector, then a body.
			if err := decodeBlockType(data, &ptr, expr); err != nil {
				return nil, 0, err
			}
			numCatches, l := binary.Uvarint(data[ptr:])
			if l <= 0 {
				return nil, 0, fmt.Errorf("Invalid try_table catch count at pc %d", expr.PC)
			}
			ptr += l
			for ci := 0; ci < int(numCatches); ci++ {
				if ptr >= len(data) {
					return nil, 0, fmt.Errorf("Truncated try_table catch vector at pc %d", expr.PC)
				}
				kind := data[ptr]
				ptr++
				c := Catch{Kind: CatchKind(kind), TagIndex: -1}
				switch c.Kind {
				case CatchTag, CatchTagRef:
					tagIdx, l := binary.Uvarint(data[ptr:])
					if l <= 0 {
						return nil, 0, fmt.Errorf("Invalid try_table tag at pc %d", expr.PC)
					}
					ptr += l
					labelIdx, l := binary.Uvarint(data[ptr:])
					if l <= 0 {
						return nil, 0, fmt.Errorf("Invalid try_table label at pc %d", expr.PC)
					}
					ptr += l
					c.TagIndex = int(tagIdx)
					c.LabelIndex = int(labelIdx)
				case CatchAll, CatchAllRef:
					labelIdx, l := binary.Uvarint(data[ptr:])
					if l <= 0 {
						return nil, 0, fmt.Errorf("Invalid try_table label at pc %d", expr.PC)
					}
					ptr += l
					c.LabelIndex = int(labelIdx)
				default:
					return nil, 0, fmt.Errorf("Unsupported try_table catch kind %d at pc %d", kind, expr.PC)
				}
				expr.Catches = append(expr.Catches, c)
			}
			nestCounter++
		} else if Opcode(opcode) == InstrToOpcode["throw"] {
			if ptr >= len(data) {
				return nil, 0, fmt.Errorf("Truncated throw at pc %d", expr.PC)
			}
			val, l := binary.Uvarint(data[ptr:])
			if l <= 0 {
				return nil, 0, fmt.Errorf("Invalid throw tag at pc %d", expr.PC)
			}
			ptr += l
			expr.TagIndex = int(val)
		} else if Opcode(opcode) == InstrToOpcode["end"] {
			nestCounter--
			if nestCounter == 0 {
				break
			}
		} else if Opcode(opcode) == InstrToOpcode["i32.const"] {
			val, l := encoding.DecodeSleb128(data[ptr:])
			ptr += int(l)
			expr.I32Value = int32(val)
		} else if Opcode(opcode) == InstrToOpcode["i64.const"] {
			val, l := encoding.DecodeSleb128(data[ptr:])
			ptr += int(l)
			expr.I64Value = int64(val)
		} else if Opcode(opcode) == InstrToOpcode["f32.const"] {
			ival := binary.LittleEndian.Uint32(data[ptr : ptr+4])
			val := math.Float32frombits(ival)
			ptr += 4
			expr.F32Value = float32(val)
		} else if Opcode(opcode) == InstrToOpcode["f64.const"] {
			ival := binary.LittleEndian.Uint64(data[ptr : ptr+8])
			val := math.Float64frombits(ival)
			ptr += 8
			expr.F64Value = float64(val)
		} else if Opcode(opcode) == InstrToOpcode["local.get"] ||
			Opcode(opcode) == InstrToOpcode["local.set"] ||
			Opcode(opcode) == InstrToOpcode["local.tee"] {
			val, l := binary.Uvarint(data[ptr:])
			ptr += l
			expr.LocalIndex = int(val)
		} else if Opcode(opcode) == InstrToOpcode["global.get"] ||
			Opcode(opcode) == InstrToOpcode["global.set"] {
			val, l := binary.Uvarint(data[ptr:])
			ptr += l
			expr.GlobalIndex = int(val)
		} else if Opcode(opcode) == InstrToOpcode["call"] {
			val, l := binary.Uvarint(data[ptr:])
			ptr += l
			expr.FuncIndex = int(val)
		} else if Opcode(opcode) == InstrToOpcode["call_indirect"] {
			typeIdx, l := binary.Uvarint(data[ptr:])
			ptr += l
			tableIdx, l := binary.Uvarint(data[ptr:])
			ptr += l
			expr.TypeIndex = int(typeIdx)
			expr.TableIndex = int(tableIdx)
		} else if Opcode(opcode) == ExtendedOpcodeFC {
			opcode2, l := binary.Uvarint(data[ptr:])
			ptr += l
			expr.OpcodeExt = int(opcode2)
			// Now deal with opcode2...
			if int(opcode2) == instrToOpcodeFC["memory.copy"] {
				// For now we expect two 0 bytes.
				ptr += 2
			} else if int(opcode2) == instrToOpcodeFC["memory.fill"] {
				// For now we expect one 0 byte.
				ptr++
			} else if int(opcode2) == instrToOpcodeFC["i32.trunc_sat_f32_s"] ||
				int(opcode2) == instrToOpcodeFC["i32.trunc_sat_f32_u"] ||
				int(opcode2) == instrToOpcodeFC["i32.trunc_sat_f64_s"] ||
				int(opcode2) == instrToOpcodeFC["i32.trunc_sat_f64_u"] ||
				int(opcode2) == instrToOpcodeFC["i64.trunc_sat_f32_s"] ||
				int(opcode2) == instrToOpcodeFC["i64.trunc_sat_f32_u"] ||
				int(opcode2) == instrToOpcodeFC["i64.trunc_sat_f64_s"] ||
				int(opcode2) == instrToOpcodeFC["i64.trunc_sat_f64_u"] {

			} else {
				return nil, 0, fmt.Errorf("Unsupported opcode 0xfc %d at pc %d", opcode2, pc+uint64(opptr))
			}

		} else {
			ptr--
			return nil, 0, fmt.Errorf("Unsupported opcode %d at pc %d", data[ptr], pc+uint64(ptr))
		}

		expr.PCNext = pc + uint64(ptr)

		// Add it on to the list...
		exps = append(exps, expr)
	}
	return exps, ptr, nil
}
