// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package checklicenses

import (
	"bytes"
	"compress/gzip"
	"encoding/json"
	"fmt"
	"html"
	"io/ioutil"
	"sort"
	"strings"
	"text/template"

	"go.fuchsia.dev/fuchsia/tools/check-licenses/templates"
)

// saveToOutputFile writes to output the serialized licenses.
//
// It writes an uncompressed version too if a compressed version is requested.
func saveToOutputFile(path string, licenses *Licenses, config *Config) error {
	// Sort the licenses in alphabetical order for consistency.
	sort.Slice(licenses.licenses, func(i, j int) bool { return licenses.licenses[i].Category < licenses.licenses[j].Category })

	var used []*License
	var unused []*License
	for _, l := range licenses.licenses {
		isUsed := false
		for _, m := range l.matches {
			if m.Used {
				isUsed = true
				break
			}
		}
		if isUsed {
			used = append(used, l)
		} else {
			unused = append(unused, l)
		}
	}
	for _, n := range licenses.notices {
		used = append(used, n)
	}

	var output []byte
	var err error
	switch {
	case strings.HasSuffix(path, ".txt") || strings.HasSuffix(path, ".txt.gz"):
		output, err = createTextOutput(used, unused, config)
	case strings.HasSuffix(path, ".html") || strings.HasSuffix(path, ".html.gz"):
		output, err = createHtmlOutput(used, unused, config)
	case strings.HasSuffix(path, ".json") || strings.HasSuffix(path, ".json.gz"):
		output, err = createJsonOutput(used, unused, config)
	default:
		err = fmt.Errorf("invalid output suffix %s", path)
	}
	if err != nil {
		return err
	}

	// Special handling for compressed file.
	const gz = ".gz"
	if strings.HasSuffix(path, gz) {
		// First write uncompressed, then compressed.
		if err := ioutil.WriteFile(path[:len(path)-len(gz)], output, 0666); err != nil {
			return err
		}
		d, err := compressGZ(output)
		if err != nil {
			return err
		}
		return ioutil.WriteFile(path, d, 0666)
	}

	return ioutil.WriteFile(path, output, 0666)
}

// This struct is used to outuput the summary file
type LicenseSummary struct {
	Name       string
	Categories []string
}

// outputSummary writes license summary to a file
func outputSummary(path string, file_tree *FileTree) error {
	// Construct a map of license name to its category so we can dedup
	licCat := map[string]map[string]bool{}

	for tree := range file_tree.getFileTreeIterator() {
		for name, licenses := range tree.SingleLicenseFiles {
			if _, ok := licCat[name]; !ok {
				licCat[name] = map[string]bool{}
			}

			for _, lic := range licenses {
				licCat[name][lic.Category] = true
			}
		}
	}

	// Converted to a sorted slice
	sortedSummaries := make([]*LicenseSummary, len(licCat))
	i := 0
	for name, categories := range licCat {
		sortedCategories := make([]string, len(categories))
		j := 0
		for cat := range categories {
			sortedCategories[j] = cat
			j++
		}
		sort.Strings(sortedCategories)
		sortedSummaries[i] = &LicenseSummary{
			Name:       name,
			Categories: sortedCategories,
		}
		i++
	}

	sort.Slice(sortedSummaries, func(i, j int) bool { return sortedSummaries[i].Name < sortedSummaries[j].Name })

	output, err := createSummaryOutput(sortedSummaries)
	if err != nil {
		return err
	}

	return ioutil.WriteFile(path, output, 0666)
}

// The following structs are used to serialize data to JSON.
type Output struct {
	Unused UnusedLicenses `json:"unused"`
	Used   []UsedLicense  `json:"used"`
}

type UnusedLicenses struct {
	Categories []string `json:"categories"`
}

type UsedLicense struct {
	Copyrights []string `json:"copyrights"`
	Category   string   `json:"category"`
	Files      string   `json:"files"`
	Text       string   `json:"text"`
}

func createJsonOutput(usedLicenses []*License, unusedLicenses []*License, config *Config) ([]byte, error) {
	output := Output{}
	for _, l := range unusedLicenses {
		category := getCategory(l)
		output.Unused.Categories = append(output.Unused.Categories, category)
	}

	for _, l := range usedLicenses {
		category := getCategory(l)
		for _, match := range l.matches {
			if !match.Used {
				continue
			}

			copyrights := []string{}
			for c := range match.Copyrights {
				trim := strings.TrimSpace(c)
				if trim != "" {
					copyrights = append(copyrights, trim)
				}
			}
			sort.Strings(copyrights)

			files := ""
			if config.PrintFiles {
				files = getFilesFromMatch(match)
			}
			text := match.Text
			licenseOutput := UsedLicense{
				Copyrights: copyrights,
				Category:   category,
				Files:      files,
				Text:       text,
			}
			output.Used = append(output.Used, licenseOutput)
		}
	}

	return json.Marshal(output)
}

func createTextOutput(usedLicenses []*License, unusedLicenses []*License, config *Config) ([]byte, error) {
	data := struct {
		Used   []*License
		Unused []*License
	}{}
	data.Used = usedLicenses
	data.Unused = unusedLicenses

	buf := bytes.Buffer{}
	tmpl := template.Must(template.New("name").Funcs(getFuncMap(config)).Parse(templates.TemplateTxt))
	if err := tmpl.Execute(&buf, data); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

// TODO(omerlevran): Use html/template instead of text/template.
// text/template is inherently unsafe to generate HTML.
func createHtmlOutput(usedLicenses []*License, unusedLicenses []*License, config *Config) ([]byte, error) {
	data := struct {
		Used   []*License
		Unused []*License
	}{}
	data.Used = usedLicenses
	data.Unused = unusedLicenses

	buf := bytes.Buffer{}
	tmpl := template.Must(template.New("name").Funcs(getFuncMap(config)).Parse(templates.TemplateHtml))
	if err := tmpl.Execute(&buf, data); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

func createSummaryOutput(summaries []*LicenseSummary) ([]byte, error) {
	data := struct {
		Summaries []*LicenseSummary
	}{}
	data.Summaries = summaries

	buf := bytes.Buffer{}
	tmpl := template.Must(template.New("name").Funcs(template.FuncMap{
		"getCategories": func(summary *LicenseSummary) string {
			return "\"" + strings.Join(summary.Categories, "\n") + "\""
		},
	}).Parse(templates.TemplateSummary))
	if err := tmpl.Execute(&buf, data); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

// compressGZ returns the compressed buffer with gzip format.
func compressGZ(d []byte) ([]byte, error) {
	buf := bytes.Buffer{}
	zw := gzip.NewWriter(&buf)
	if _, err := zw.Write(d); err != nil {
		return nil, err
	}
	if err := zw.Close(); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

func getCategory(l *License) string {
	return strings.TrimSuffix(l.Category, ".lic")
}

func getAuthors(l *License) []string {
	var authors []string
	for author := range l.matches {
		authors = append(authors, author)
	}
	sort.Strings(authors)
	return authors
}

func getHTMLText(m *Match) string {
	txt := m.Text
	txt = html.EscapeString(txt)
	txt = strings.Replace(txt, "\n", "<br />\n", -1)
	return txt
}

func getMatches(l *License) []*Match {
	sortedList := []*Match{}
	for _, m := range l.matches {
		sortedList = append(sortedList, m)
	}
	sort.Sort(matchByText(sortedList))

	return sortedList
}

func getFilesFromMatch(m *Match) string {
	result := ""
	if len(m.Files) > 0 {
		result += "Files:\n"
		files := make([]string, len(m.Files))
		i := 0
		for k := range m.Files {
			files[i] = k
			i++
		}
		sort.Strings(files)
		for _, s := range files {
			result += " -> " + s + "\n"
		}
	}
	return result
}

func getProjectsFromMatch(m *Match) string {
	result := ""
	if len(m.Projects) > 0 {
		result += "Projects:\n"
		projects := make([]string, len(m.Projects))
		i := 0
		for k := range m.Projects {
			projects[i] = k
			i++
		}
		sort.Strings(projects)
		for _, s := range projects {
			result += " -> " + s + "\n"
		}
	}
	return result
}

func getFiles(l *License, author string) []string {
	if m, ok := l.matches[author]; ok {
		if len(m.Files) > 0 {
			files := make([]string, len(m.Files))
			i := 0
			for k := range m.Files {
				files[i] = k
				i++
			}
			sort.Strings(files)
			return files
		}
	}

	return []string{}
}

func getEscapedText(l *License, author string) string {
	return strings.Replace(l.matches[author].Text, "\"", "\\\"", -1)
}

func getCopyrights(m *Match) string {
	sortedList := []string{}
	for c := range m.Copyrights {
		trim := strings.TrimSpace(c)
		if trim != "" {
			sortedList = append(sortedList, trim)
		}
	}
	sort.Strings(sortedList)

	result := ""
	for _, s := range sortedList {
		result += s + "\n"
	}
	return result
}

func getFuncMap(config *Config) template.FuncMap {
	return template.FuncMap{
		"getPattern": func(l *License) string {
			return l.pattern.String()
		},
		"getText": func(l *License, author string) string {
			return l.matches[author].Text
		},
		"getEscapedText": getEscapedText,
		"getCategory":    getCategory,
		"getFiles":       getFiles,
		"getAuthors":     getAuthors,
		"getHTMLText":    getHTMLText,
		"getMatches":     getMatches,
		"getFilesFromMatch": func(m *Match) string {
			files := ""
			if config.PrintFiles {
				files = getFilesFromMatch(m)
			}
			return files
		},
		"getProjectsFromMatch": func(m *Match) string {
			projects := ""
			if config.PrintProjects {
				projects = getProjectsFromMatch(m)
			}
			return projects
		},
		"getCopyrights": getCopyrights,
	}
}
