{% include "docs/contribute/governance/rfcs/_common/_rfc_header.md" %}

# Fuchsia RFCs

The Fuchsia RFC process is intended to provide a consistent and transparent path
for making project-wide, technical decisions. For example, the RFC process can
be used to evolve the project roadmap and the system architecture.

The RFC process evolves over time, and can be read here in its [detailed current
form](rfc_process.md). It is also summarized below.

## Summary of the process

- Review [when to use the process](rfc_process.md#when-to-use-the-process) i.e.
  the criteria for requiring an RFC.
- Socialize your proposal.
- [Draft](rfc_process.md#draft) your RFC using this [template](TEMPLATE.md).
  When ready to iterate, upload your CL and share it broadly to
  <eng-council-discuss@fuchsia.dev>. See also [creating an RFC](create_rfc.md).
- [Iterate](rfc_process.md#iterate) on your proposal with appropriate
  stakeholders.
- As conversations on your proposal converge, and stakeholders indicate their
  support (+1 on the CL), or their opposition (-1 on the CL), email
  <eng-council@fuchsia.dev> to ask the Eng Council to move your proposal to
  [Last Call](rfc_process.md#last-call).
- After a waiting period of at least 7 days, the Eng Council will accept or
  reject your proposal, or ask that you iterate with stakeholders further.
  - If your RFC is accepted, a member of the Eng Council will comment on your
    change stating that the RFC is accepted, will assign the RFC a number and
    mark your change Code-Review +2. Your RFC can now be landed.
  - If your RFC is rejected, a member of the Eng Council will draft a rejection
    rationale with you for inclusion in the RFC.

## Summary of the process (deck)

<iframe src="//www.slideshare.net/slideshow/embed_code/key/9hgFMOmVfbDFsI" frameborder="0" marginwidth="0" marginheight="0" scrolling="no" style="width: 595px; height: 375px; border:1px solid #CCC; border-width:1px; margin-bottom:5px; max-width: 100%;" allowfullscreen></iframe>

## Proposals

### Active RFCs

[Gerrit link](https://fuchsia-review.googlesource.com/q/dir:docs/contribute/governance/rfcs+is:open)

### Finalized RFCs

<div class="form-checkbox">
<devsite-expandable id="rfc-area">
  <h4 class="showalways">RFC area</h4>
<form id="filter-checkboxes-reset">
  {%- for area in areas %}
    {%- set found=false %}
    {%- for rfc in rfcs %}
        {%- for rfca in rfc.area %}
          {%- if rfca == area %}
            {%- set found=true %}
          {%- endif %}
        {%- endfor %}
    {%- endfor %}
    {%- if found %}
      <div class="checkbox-div">
        <input type="checkbox" id="checkbox-reset-{{ area|lower|replace(' ','-')|replace('.','-')  }}" checked>
        <label for="checkbox-reset-{{ area|lower|replace(' ','-')|replace('.','-') }}">{{ area }}</label>
      </div>
    {%- endif %}
  {%- endfor %}
  <br>
  <br>
  <button class="select-all">Select all</button>
  <button class="clear-all">Clear all</button>
  <hr>
  <div class="see-rfcs">
    <div class="rfc-left">
      <p><a href="#accepted-rfc">Accepted RFCs</a></p>
    </div>
    <div class="rfc-right">
      <p><a href="#rejected-rfc">Rejected RFCs</a></p>
    </div>
  </div>
</form>
</devsite-expandable>

<a name="accepted-rfc"><h3 class="hide-from-toc">Accepted</h3></a>
{% include "docs/contribute/governance/rfcs/_common/_index_table_header.md" %}
{%- for rfc in rfcs | sort(attribute='name') %}
    {%- if rfc.status == "Accepted" %}
        {% include "docs/contribute/governance/rfcs/_common/_index_table_body.md" %}
    {%- endif %}
{%- endfor %}
{% include "docs/contribute/governance/rfcs/_common/_index_table_footer.md" %}

<a name="rejected-rfc"><h3 class="hide-from-toc">Rejected</h3></a>
{% include "docs/contribute/governance/rfcs/_common/_index_table_header.md" %}
{%- for rfc in rfcs | sort(attribute='name') %}
    {%- if rfc.status == "Rejected" %}
        {% include "docs/contribute/governance/rfcs/_common/_index_table_body.md" %}
    {%- endif %}
{%- endfor %}
{% include "docs/contribute/governance/rfcs/_common/_index_table_footer.md" %}

{# This div is used to close the filter that is initialized above #}
</div>

## Subscribing to RFCs

You can subscribe for email notifications when new RFCs are uploaded using
[Gerrit Notifications][gerrit-notifications]. A search expression for
`docs/contribute/governance/rfcs` should capture all new RFC changes.

![Gerrit settings screenshot demonstrating the
above](resources/gerrit_notifications.png)

[gerrit-notifications]: https://fuchsia-review.googlesource.com/settings/#Notifications
