<%include file="header.mako" />

% for layer in data.layers:
copy("${layer.name}_config") {
  sources = [
    "${layer.config}",
  ]
  outputs = [
    <%text>"${root_gen_dir}/</%text>${layer.config}",
  ]
}

% if layer.binary:
copy("${layer.name}_lib") {
  sources = [
    % if 'MISSING' in layer.binary:
    # Layer ${layer.name} has a config but does not have a binary.
    # This path will result in a build error.
    % endif
    "${layer.binary}",
  ]
  outputs = [
    <%text>"${root_out_dir}/lib/{{source_file_part}}",</%text>
  ]
}
% endif

group("${layer.name}") {
  data_deps = [
    ":${layer.name}_config",
    % if layer.binary:
    ":${layer.name}_lib",
    % endif
    % for dep in layer.data_deps:
    "${dep}",
    % endfor
  ]
}

% endfor
group("all"){
  data_deps = [
  % for layer in data.layers:
    ":${layer.name}",
  %endfor%
  ]
}
