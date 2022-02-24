// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package dart

import (
	"fmt"
	"strconv"
	"strings"

	gidlir "go.fuchsia.dev/fuchsia/tools/fidl/gidl/ir"
	gidlmixer "go.fuchsia.dev/fuchsia/tools/fidl/gidl/mixer"
	"go.fuchsia.dev/fuchsia/tools/fidl/lib/fidlgen"
)

func buildHandleDefs(defs []gidlir.HandleDef) string {
	if len(defs) == 0 {
		return ""
	}
	var builder strings.Builder
	builder.WriteString("[\n")
	for i, d := range defs {
		builder.WriteString("HandleDef(")
		switch d.Subtype {
		case fidlgen.Event:
			builder.WriteString(fmt.Sprint("HandleSubtype.event, "))
		case fidlgen.Channel:
			builder.WriteString(fmt.Sprint("HandleSubtype.channel, "))
		default:
			panic(fmt.Sprintf("unknown handle subtype: %v", d.Subtype))
		}
		// Write indices corresponding to the .gidl file handle_defs block.
		builder.WriteString(fmt.Sprintf("%d),// #%d\n", d.Rights, i))
	}
	builder.WriteString("]")
	return builder.String()
}

func buildHandleValues(handles []gidlir.Handle) string {
	var builder strings.Builder
	builder.WriteString("[\n")
	for _, h := range handles {
		builder.WriteString(fmt.Sprintf("%s,", buildHandleValue(h)))
	}
	builder.WriteString("]")
	return builder.String()
}

func buildUnknownTableData(fields []gidlir.Field) string {
	if len(fields) == 0 {
		return "null"
	}
	var builder strings.Builder
	builder.WriteString("{\n")
	for _, field := range fields {
		unknownData := field.Value.(gidlir.UnknownData)
		builder.WriteString(fmt.Sprintf(
			"%d: fidl.UnknownRawData(\n%s,\n%s\n),",
			field.Key.UnknownOrdinal,
			buildBytes(unknownData.Bytes),
			buildHandleValues(unknownData.Handles)))
	}
	builder.WriteString("}")
	return builder.String()
}

func visit(value gidlir.Value, decl gidlmixer.Declaration) string {
	switch value := value.(type) {
	case bool:
		return strconv.FormatBool(value)
	case int64, uint64, float64:
		switch decl := decl.(type) {
		case *gidlmixer.IntegerDecl, *gidlmixer.FloatDecl:
			return fmt.Sprintf("%#v", value)
		case gidlmixer.NamedDeclaration:
			return fmt.Sprintf("%s.ctor(%#v)", typeName(decl), value)
		}
	case gidlir.RawFloat:
		switch decl.(*gidlmixer.FloatDecl).Subtype() {
		case fidlgen.Float32:
			return fmt.Sprintf("Uint32List.fromList([%#x]).buffer.asByteData().getFloat32(0, Endian.host)", value)
		case fidlgen.Float64:
			return fmt.Sprintf("Uint64List.fromList([%#x]).buffer.asByteData().getFloat64(0, Endian.host)", value)
		}
	case string:
		return toDartStr(value)
	case gidlir.HandleWithRights:
		rawHandle := buildHandleValue(value.Handle)
		handleDecl := decl.(*gidlmixer.HandleDecl)
		switch handleDecl.Subtype() {
		case fidlgen.Handle:
			return rawHandle
		case fidlgen.Channel:
			return fmt.Sprintf("Channel(%s)", rawHandle)
		case fidlgen.Event:
			// Dart does not support events, so events are mapped to bare handles
			return rawHandle
		default:
			panic(fmt.Sprintf("unknown handle subtype: %v", handleDecl.Subtype()))
		}
	case gidlir.Record:
		switch decl := decl.(type) {
		case *gidlmixer.StructDecl:
			return onRecord(value, decl)
		case *gidlmixer.TableDecl:
			return onRecord(value, decl)
		case *gidlmixer.UnionDecl:
			return onUnion(value, decl)
		}
	case []gidlir.Value:
		switch decl := decl.(type) {
		case *gidlmixer.ArrayDecl:
			return onList(value, decl)
		case *gidlmixer.VectorDecl:
			return onList(value, decl)
		}
	case nil:
		if !decl.IsNullable() {
			panic(fmt.Sprintf("got nil for non-nullable type: %T", decl))
		}
		return "null"

	}
	panic(fmt.Sprintf("not implemented: %T", value))
}

func onRecord(value gidlir.Record, decl gidlmixer.RecordDeclaration) string {
	var args []string
	var unknownTableFields []gidlir.Field
	for _, field := range value.Fields {
		if field.Key.IsUnknown() {
			unknownTableFields = append(unknownTableFields, field)
			continue
		}
		fieldDecl, ok := decl.Field(field.Key.Name)
		if !ok {
			panic(fmt.Sprintf("field %s not found", field.Key.Name))
		}
		val := visit(field.Value, fieldDecl)
		args = append(args, fmt.Sprintf("%s: %s", fidlgen.ToLowerCamelCase(field.Key.Name), val))
	}
	if len(unknownTableFields) > 0 {
		args = append(args,
			fmt.Sprintf("$unknownData: %s", buildUnknownTableData(unknownTableFields)))
	}
	return fmt.Sprintf("%s(%s)", fidlgen.ToUpperCamelCase(value.Name), strings.Join(args, ", "))
}

func onUnion(value gidlir.Record, decl *gidlmixer.UnionDecl) string {
	for _, field := range value.Fields {
		if field.Key.IsUnknown() {
			unknownData := field.Value.(gidlir.UnknownData)
			return fmt.Sprintf(
				"%s.with$UnknownData(%d, fidl.UnknownRawData(%s, %s))",
				value.Name,
				field.Key.UnknownOrdinal,
				buildBytes(unknownData.Bytes),
				buildHandleValues(unknownData.Handles))
		}
		fieldDecl, ok := decl.Field(field.Key.Name)
		if !ok {
			panic(fmt.Sprintf("field %s not found", field.Key.Name))
		}
		val := visit(field.Value, fieldDecl)
		return fmt.Sprintf("%s.with%s(%s)", value.Name, fidlgen.ToUpperCamelCase(field.Key.Name), val)
	}
	// Not currently possible to construct a union in dart with an invalid value.
	panic("unions must have a value set")
}

func onList(value []gidlir.Value, decl gidlmixer.ListDeclaration) string {
	var elements []string
	elemDecl := decl.Elem()
	for _, item := range value {
		elements = append(elements, visit(item, elemDecl))
	}
	if integerDecl, ok := elemDecl.(*gidlmixer.IntegerDecl); ok {
		typeName := fidlgen.ToUpperCamelCase(string(integerDecl.Subtype()))
		return fmt.Sprintf("%sList.fromList([%s])", typeName, strings.Join(elements, ", "))
	}
	if floatDecl, ok := elemDecl.(*gidlmixer.FloatDecl); ok {
		typeName := fidlgen.ToUpperCamelCase(string(floatDecl.Subtype()))
		return fmt.Sprintf("%sList.fromList([%s])", typeName, strings.Join(elements, ", "))
	}
	return fmt.Sprintf("[%s]", strings.Join(elements, ", "))
}

func typeName(decl gidlmixer.NamedDeclaration) string {
	parts := strings.Split(decl.Name(), "/")
	lastPart := parts[len(parts)-1]
	return dartTypeName(lastPart)
}

func buildHandleValue(handle gidlir.Handle) string {
	return fmt.Sprintf("handles[%d]", handle)
}
