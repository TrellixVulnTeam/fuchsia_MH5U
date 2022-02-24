// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package hlcpp

import (
	"bytes"
	"encoding/hex"
	"fmt"
	"strconv"
	"strings"

	gidlir "go.fuchsia.dev/fuchsia/tools/fidl/gidl/ir"
	gidlmixer "go.fuchsia.dev/fuchsia/tools/fidl/gidl/mixer"
	"go.fuchsia.dev/fuchsia/tools/fidl/lib/fidlgen"
)

func escapeStr(value string) string {
	if fidlgen.PrintableASCII(value) {
		return strconv.Quote(value)
	}
	var (
		buf    bytes.Buffer
		src    = []byte(value)
		dstLen = hex.EncodedLen(len(src))
		dst    = make([]byte, dstLen)
	)
	hex.Encode(dst, src)
	buf.WriteRune('"')
	for i := 0; i < dstLen; i += 2 {
		buf.WriteString("\\x")
		buf.WriteByte(dst[i])
		buf.WriteByte(dst[i+1])
	}
	buf.WriteRune('"')
	return buf.String()
}

func buildHandleDef(def gidlir.HandleDef) string {
	switch def.Subtype {
	case fidlgen.Channel:
		return fmt.Sprintf("fidl::test::util::CreateChannel(%d)", def.Rights)
	case fidlgen.Event:
		return fmt.Sprintf("fidl::test::util::CreateEvent(%d)", def.Rights)
	default:
		panic(fmt.Sprintf("unsupported handle subtype: %s", def.Subtype))
	}
}

func handleType(subtype fidlgen.HandleSubtype) string {
	switch subtype {
	case fidlgen.Channel:
		return "ZX_OBJ_TYPE_CHANNEL"
	case fidlgen.Event:
		return "ZX_OBJ_TYPE_EVENT"
	default:
		panic(fmt.Sprintf("unsupported handle subtype: %s", subtype))
	}
}

func BuildHandleDefs(defs []gidlir.HandleDef) string {
	if len(defs) == 0 {
		return ""
	}
	var builder strings.Builder
	builder.WriteString("std::vector<zx_handle_t>{\n")
	for i, d := range defs {
		builder.WriteString(buildHandleDef(d))
		// Write indices corresponding to the .gidl file handle_defs block.
		builder.WriteString(fmt.Sprintf(", // #%d\n", i))
	}
	builder.WriteString("}")
	return builder.String()
}

func BuildHandleInfoDefs(defs []gidlir.HandleDef) string {
	if len(defs) == 0 {
		return ""
	}
	var builder strings.Builder
	builder.WriteString("std::vector<zx_handle_info_t>{\n")
	for i, d := range defs {
		builder.WriteString(fmt.Sprintf(`
// #%d
zx_handle_info_t{
	.handle = %s,
	.type = %s,
	.rights = %d,
	.unused = 0u,
},
`, i, buildHandleDef(d), handleType(d.Subtype), d.Rights))
	}
	builder.WriteString("}")
	return builder.String()
}

func newCppValueBuilder() cppValueBuilder {
	return cppValueBuilder{}
}

type cppValueBuilder struct {
	strings.Builder

	varidx          int
	handleExtractOp string
}

func (b *cppValueBuilder) newVar() string {
	b.varidx++
	return fmt.Sprintf("v%d", b.varidx)
}

func (b *cppValueBuilder) visit(value gidlir.Value, decl gidlmixer.Declaration) string {
	switch value := value.(type) {
	case bool:
		return fmt.Sprintf("%t", value)
	case int64:
		intString := fmt.Sprintf("%dll", value)
		if value == -9223372036854775808 {
			intString = "-9223372036854775807ll - 1"
		}
		switch decl := decl.(type) {
		case *gidlmixer.IntegerDecl:
			return intString
		case *gidlmixer.BitsDecl:
			return fmt.Sprintf("%s(%s)", typeName(decl), intString)
		case *gidlmixer.EnumDecl:
			return fmt.Sprintf("%s(%s)", typeName(decl), intString)
		}
	case uint64:
		switch decl := decl.(type) {
		case *gidlmixer.IntegerDecl:
			return fmt.Sprintf("%dull", value)
		case *gidlmixer.BitsDecl:
			return fmt.Sprintf("%s(%dull)", typeName(decl), value)
		case *gidlmixer.EnumDecl:
			return fmt.Sprintf("%s(%dull)", typeName(decl), value)
		}
	case float64:
		switch decl := decl.(type) {
		case *gidlmixer.FloatDecl:
			switch decl.Subtype() {
			case fidlgen.Float32:
				s := fmt.Sprintf("%g", value)
				if strings.Contains(s, ".") {
					return fmt.Sprintf("%sf", s)
				} else {
					return s
				}
			case fidlgen.Float64:
				return fmt.Sprintf("%g", value)
			}
		}
	case gidlir.RawFloat:
		switch decl.(*gidlmixer.FloatDecl).Subtype() {
		case fidlgen.Float32:
			return fmt.Sprintf("([] { uint32_t u = %#b; float f; memcpy(&f, &u, 4); return f; })()", value)
		case fidlgen.Float64:
			return fmt.Sprintf("([] { uint64_t u = %#b; double d; memcpy(&d, &u, 8); return d; })()", value)
		}
	case string:
		return fmt.Sprintf("%s(%s, %d)", typeName(decl), escapeStr(value), len(value))
	case gidlir.HandleWithRights:
		return fmt.Sprintf("%s(handle_defs[%d]%s)", typeName(decl), value.Handle, b.handleExtractOp)
	case gidlir.Record:
		return b.visitRecord(value, decl.(gidlmixer.RecordDeclaration))
	case []gidlir.Value:
		switch decl := decl.(type) {
		case *gidlmixer.ArrayDecl:
			return b.visitArray(value, decl)
		case *gidlmixer.VectorDecl:
			return b.visitVector(value, decl)
		}
	case nil:
		return fmt.Sprintf("%s()", typeName(decl))
	}
	panic(fmt.Sprintf("not implemented: %T", value))
}

func (b *cppValueBuilder) visitRecord(value gidlir.Record, decl gidlmixer.RecordDeclaration) string {
	containerVar := b.newVar()
	nullable := decl.IsNullable()
	if nullable {
		b.Builder.WriteString(fmt.Sprintf(
			"%s %s = std::make_unique<%s>();\n", typeName(decl), containerVar, declName(decl)))
	} else {
		b.Builder.WriteString(fmt.Sprintf("%s %s;\n", typeName(decl), containerVar))
	}

	_, isTable := decl.(*gidlmixer.TableDecl)
	for _, field := range value.Fields {
		accessor := "."
		if nullable {
			accessor = "->"
		}
		b.Builder.WriteString("\n")

		if field.Key.IsUnknown() {
			if isTable {
				unknownData := field.Value.(gidlir.UnknownData)
				if decl.IsResourceType() {
					b.Builder.WriteString(fmt.Sprintf(`::fidl::UnknownData _data = {
	.bytes=%s,
	.handles=%s
};
%s%sSetUnknownDataEntry(%dlu, std::move(_data));
`, BuildBytes(unknownData.Bytes), buildHandles(unknownData.Handles, b.handleExtractOp), containerVar, accessor, field.Key.UnknownOrdinal))
				} else {
					b.Builder.WriteString(fmt.Sprintf(
						"%s%sSetUnknownDataEntry(%dlu, %s);\n",
						containerVar, accessor, field.Key.UnknownOrdinal, BuildBytes(unknownData.Bytes)))
				}
			} else {
				unknownData := field.Value.(gidlir.UnknownData)
				if decl.IsResourceType() {
					b.Builder.WriteString(fmt.Sprintf(
						"%s%sSetUnknownData(static_cast<fidl_xunion_tag_t>(%dlu), %s, %s);\n",
						containerVar, accessor, field.Key.UnknownOrdinal, BuildBytes(unknownData.Bytes),
						buildHandles(unknownData.Handles, b.handleExtractOp)))
				} else {
					b.Builder.WriteString(fmt.Sprintf(
						"%s%sSetUnknownData(static_cast<fidl_xunion_tag_t>(%dlu), %s);\n",
						containerVar, accessor, field.Key.UnknownOrdinal, BuildBytes(unknownData.Bytes)))
				}
			}
			continue
		}

		fieldDecl, ok := decl.Field(field.Key.Name)
		if !ok {
			panic(fmt.Sprintf("field %s not found", field.Key.Name))
		}
		fieldVar := b.visit(field.Value, fieldDecl)

		switch decl.(type) {
		case *gidlmixer.StructDecl:
			b.Builder.WriteString(fmt.Sprintf(
				"%s%s%s = %s;\n", containerVar, accessor, field.Key.Name, fieldVar))
		default:
			b.Builder.WriteString(fmt.Sprintf(
				"%s%sset_%s(%s);\n", containerVar, accessor, field.Key.Name, fieldVar))
		}
	}
	return fmt.Sprintf("std::move(%s)", containerVar)
}

func (b *cppValueBuilder) visitArray(value []gidlir.Value, decl *gidlmixer.ArrayDecl) string {
	var elements []string
	elemDecl := decl.Elem()
	for _, item := range value {
		elements = append(elements, fmt.Sprintf("%s", b.visit(item, elemDecl)))
	}
	// Populate the array using aggregate initialization.
	return fmt.Sprintf("%s{%s}",
		typeName(decl), strings.Join(elements, ", "))
}

func (b *cppValueBuilder) visitVector(value []gidlir.Value, decl *gidlmixer.VectorDecl) string {
	elemDecl := decl.Elem()
	var elements []string
	for _, item := range value {
		elements = append(elements, b.visit(item, elemDecl))
	}
	if _, ok := elemDecl.(gidlmixer.PrimitiveDeclaration); ok {
		// Populate the vector using aggregate initialization.
		if decl.IsNullable() {
			return fmt.Sprintf("%s{{%s}}", typeName(decl), strings.Join(elements, ", "))
		}
		return fmt.Sprintf("%s{%s}", typeName(decl), strings.Join(elements, ", "))
	}
	vectorVar := b.newVar()
	// Populate the vector using push_back. We can't use an initializer list
	// because they always copy, which breaks if the element is a unique_ptr.
	b.Builder.WriteString(fmt.Sprintf("%s %s;\n", typeName(decl), vectorVar))
	if decl.IsNullable() && value != nil {
		b.Builder.WriteString(fmt.Sprintf("%s.emplace();\n", vectorVar))
	}
	accessor := "."
	if decl.IsNullable() {
		accessor = "->"
	}
	for _, element := range elements {
		b.Builder.WriteString(fmt.Sprintf("%s%spush_back(%s);\n", vectorVar, accessor, element))
	}
	return fmt.Sprintf("std::move(%s)", vectorVar)
}

func typeName(decl gidlmixer.Declaration) string {
	switch decl := decl.(type) {
	case gidlmixer.PrimitiveDeclaration:
		return primitiveTypeName(decl.Subtype())
	case gidlmixer.NamedDeclaration:
		if decl.IsNullable() {
			return fmt.Sprintf("std::unique_ptr<%s>", declName(decl))
		}
		return declName(decl)
	case *gidlmixer.StringDecl:
		if decl.IsNullable() {
			return "::fidl::StringPtr"
		}
		return "std::string"
	case *gidlmixer.ArrayDecl:
		return fmt.Sprintf("std::array<%s, %d>", typeName(decl.Elem()), decl.Size())
	case *gidlmixer.VectorDecl:
		if decl.IsNullable() {
			return fmt.Sprintf("::fidl::VectorPtr<%s>", typeName(decl.Elem()))
		}
		return fmt.Sprintf("std::vector<%s>", typeName(decl.Elem()))
	case *gidlmixer.HandleDecl:
		switch decl.Subtype() {
		case fidlgen.Handle:
			return "zx::handle"
		case fidlgen.Channel:
			return "zx::channel"
		case fidlgen.Event:
			return "zx::event"
		default:
			panic(fmt.Sprintf("Handle subtype not supported %s", decl.Subtype()))
		}
	default:
		panic("unhandled case")
	}
}

func declName(decl gidlmixer.NamedDeclaration) string {
	parts := strings.Split(decl.Name(), "/")
	library_parts := strings.Split(parts[0], ".")
	return strings.Join(append(library_parts, parts[1]), "::")
}

func primitiveTypeName(subtype fidlgen.PrimitiveSubtype) string {
	switch subtype {
	case fidlgen.Bool:
		return "bool"
	case fidlgen.Uint8, fidlgen.Uint16, fidlgen.Uint32, fidlgen.Uint64,
		fidlgen.Int8, fidlgen.Int16, fidlgen.Int32, fidlgen.Int64:
		return fmt.Sprintf("%s_t", subtype)
	case fidlgen.Float32:
		return "float"
	case fidlgen.Float64:
		return "double"
	default:
		panic(fmt.Sprintf("unexpected subtype %s", subtype))
	}
}

func BuildBytes(bytes []byte) string {
	var builder strings.Builder
	builder.WriteString("std::vector<uint8_t>{")
	for i, b := range bytes {
		if i%8 == 0 {
			builder.WriteString("\n")
		}
		builder.WriteString(fmt.Sprintf("0x%02x,", b))
	}
	builder.WriteString("\n}")
	return builder.String()
}

func buildRawHandleImpl(handles []gidlir.Handle, handleType, handleExtractOp string) string {
	if len(handles) == 0 {
		return fmt.Sprintf("std::vector<%s>{}", handleType)
	}
	var builder strings.Builder
	builder.WriteString(fmt.Sprintf("std::vector<%s>{\n", handleType))
	for i, h := range handles {
		builder.WriteString(fmt.Sprintf("handle_defs[%d]%s,", h, handleExtractOp))
		if i%8 == 7 {
			builder.WriteString("\n")
		}
	}
	builder.WriteString("}")
	return builder.String()
}

func BuildRawHandles(handles []gidlir.Handle) string {
	return buildRawHandleImpl(handles, "zx_handle_t", "")
}

func BuildRawHandlesFromHandleInfos(handles []gidlir.Handle) string {
	return buildRawHandleImpl(handles, "zx_handle_t", ".handle")
}

func BuildRawHandleInfos(handles []gidlir.Handle) string {
	return buildRawHandleImpl(handles, "zx_handle_info_t", "")
}

func BuildRawHandleDispositions(handle_dispositions []gidlir.HandleDisposition) string {
	if len(handle_dispositions) == 0 {
		return fmt.Sprintf("std::vector<zx_handle_disposition_t>{}")
	}
	var builder strings.Builder
	builder.WriteString(fmt.Sprintf("std::vector<zx_handle_disposition_t>{"))
	for _, h := range handle_dispositions {
		builder.WriteString(fmt.Sprintf(`
{
	.operation = ZX_HANDLE_OP_MOVE,
	.handle = handle_defs[%d],
	.type = %d,
	.rights = %d,
	.result = ZX_OK,
},`, h.Handle, h.Type, h.Rights))
	}
	builder.WriteString("}")
	return builder.String()
}

func buildHandles(handles []gidlir.Handle, handleExtractOp string) string {
	if len(handles) == 0 {
		return "std::vector<zx::handle>{}"
	}
	var builder strings.Builder
	// Initializer-list vectors only work for copyable types. zx::handle has no
	// copy constructor, so we use an immediately-invoked lambda instead.
	builder.WriteString("([&handle_defs] {\n")
	builder.WriteString("std::vector<zx::handle> v;\n")
	for _, h := range handles {
		builder.WriteString(fmt.Sprintf("v.emplace_back(handle_defs[%d]%s);\n", h, handleExtractOp))
	}
	builder.WriteString("return v;\n")
	builder.WriteString("})()")
	return builder.String()
}
