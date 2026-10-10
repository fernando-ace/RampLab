"use strict";

const $ = (selector, root = document) => root.querySelector(selector);
const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];
const state = { boot: null, scenario: null, view: "airport", selectedEdge: null, selectedVehicle: null, validation: null, runs: [], compare: null, compareIds:null, pollTimer: null, toastTimer: null, flightFilter:"", flightSort:"time" };
const titles = {
  airport: ["Airport overview", "Inspect the runway and taxiway network, then select a supported route to configure a closure."],
  flights: ["Flight schedule", "Review the schedule and edit fields that feed the RampLab scenario generator."],
  fleet: ["Fleet & resources", "Inspect mapped mobile equipment and change supported availability or initial positions."],
  turnarounds: ["Turnaround services", "Adjust service requirements and durations used to build the aircraft turnaround."],
  environment: ["Environment & timing", "Change the random seed and review the source schedule window and modeled assumptions."],
  disruptions: ["Disruption plan", "Add supported changes, review map targets, and enable or remove each intervention."],
  validation: ["Validation & changes", "Check the configuration against the existing airport generator before you run it."],
  results: ["Results & comparison", "Review native RampLab outputs and compare a completed variant with its baseline run."]
};
const esc = (value) => String(value ?? "").replace(/[&<>"']/g, (ch) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[ch]));
const clone = (value) => JSON.parse(JSON.stringify(value));
const activeDisruptions = () => (state.scenario?.disruptions || []).filter(item => item.enabled !== false);
const mappedRouteForEdge = (edgeId) => Object.entries(state.boot?.capabilities?.mapped_routes || {}).find(([, value]) => value === edgeId)?.[0] || null;
const all = (name) => Array.isArray(state.scenario?.[name]) ? state.scenario[name] : [];

async function api(path, body) {
  const response = await fetch(path, body ? { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) } : {});
  const payload = await response.json().catch(() => ({}));
  if (!response.ok) throw new Error(payload.error || `Request failed (${response.status}).`);
  return payload;
}

function toast(message, error = false) {
  const element = $("#toast");
  element.textContent = message;
  element.className = `toast show${error ? " error" : ""}`;
  clearTimeout(state.toastTimer);
  state.toastTimer = setTimeout(() => { element.className = "toast"; }, 3600);
}

async function loadAirport(key = "kauo") {
  try {
    const boot = await api(`/api/studio/bootstrap?airport=${encodeURIComponent(key)}`);
    state.boot = boot;
    state.scenario = boot.scenario;
    state.runs = boot.runs || [];
  state.selectedEdge = null; state.selectedVehicle=null;
    const select = $("#airport-select");
    select.innerHTML = boot.catalog.available_bases.map(item => `<option value="${esc(item.key)}" ${item.key === key ? "selected" : ""}>${esc(item.label)}</option>`).join("");
    $("#airport-code").textContent = boot.airport.id;
    $("#airport-name").textContent = boot.airport.name;
    $("#map-title").textContent = `${boot.airport.id} operational map`;
    $("#dataset-tag").textContent = boot.catalog.available_bases.find(item => item.key === key)?.provenance || "Source classification available";
    $("#scenario-name").value = state.scenario.name;
    render();
  } catch (error) { toast(error.message, true); }
}

function markEdited() {
  if (!state.scenario) return;
  state.scenario.baseline = false;
  state.validation = null;
  state.compare = null;
}

function render() {
  if (!state.boot || !state.scenario) return;
  $$(".view").forEach(view => view.classList.toggle("active", view.id === `view-${state.view}`));
  $$(".nav-item").forEach(button => button.classList.toggle("active", button.dataset.view === state.view));
  $("#view-title").textContent = titles[state.view][0];
  $("#view-description").textContent = titles[state.view][1];
  $("#scenario-name").value = state.scenario.name;
  $("#mode-tag").textContent = state.scenario.baseline ? "Baseline" : "Variant";
  $("#mode-tag").className = `badge ${state.scenario.baseline ? "blue" : "orange"}`;
  const flights = all("flights"), vehicles = all("equipment"), stands = all("gates");
  $("#flight-count").textContent = flights.length;
  $("#vehicle-count").textContent = vehicles.length;
  $("#stand-count").textContent = stands.length;
  $("#disruption-count").textContent = activeDisruptions().length;
  $("#map-disruption-count").textContent = activeDisruptions().length;
  drawMap();
  if (state.view === "airport") renderInspector();
  if (state.view === "flights") renderFlights();
  if (state.view === "fleet") renderFleet();
  if (state.view === "turnarounds") renderTurnarounds();
  if (state.view === "environment") renderEnvironment();
  if (state.view === "disruptions") renderDisruptions();
  if (state.view === "validation") renderValidation();
  if (state.view === "results") renderResults();
}

function projectMap() {
  const geometry = state.boot.geometry;
  const coordinates = geometry.nodes.map(node => [Number(node.x_m), Number(node.y_m)]);
  for (const runway of geometry.features.runways || []) {
    const theta = Number(runway.true_heading_degrees_from_first_threshold || 0) * Math.PI / 180;
    const half = Number(runway.length_m || 0) / 2;
    const x = Number(runway.center_local_m.x), y = Number(runway.center_local_m.y);
    coordinates.push([x + Math.sin(theta) * half, y + Math.cos(theta) * half]);
    coordinates.push([x - Math.sin(theta) * half, y - Math.cos(theta) * half]);
  }
  let minX = Math.min(...coordinates.map(p => p[0])), maxX = Math.max(...coordinates.map(p => p[0]));
  let minY = Math.min(...coordinates.map(p => p[1])), maxY = Math.max(...coordinates.map(p => p[1]));
  const padX = (maxX - minX) * .065, padY = (maxY - minY) * .08;
  minX -= padX; maxX += padX; minY -= padY; maxY += padY;
  const width = 1000, height = 640;
  const scale = Math.min(width / (maxX - minX), height / (maxY - minY));
  const offsetX = (width - (maxX - minX) * scale) / 2, offsetY = (height - (maxY - minY) * scale) / 2;
  return { width, height, scale, point(x, y) { return [offsetX + (x - minX) * scale, height - offsetY - (y - minY) * scale]; } };
}

function drawMap() {
  const geometry = state.boot.geometry, p = projectMap();
  const nodes = new Map(geometry.nodes.map(item => [item.id, item]));
  const closed = new Set(activeDisruptions().filter(item => item.type === "route_closure").map(item => state.boot.capabilities.mapped_routes[item.resource_id]).filter(Boolean));
  const nodeLabels={rwy36_hold:"RWY 36",twy_a_south:"Taxiway A · S",twy_a_mid:"Taxiway A · Mid",twy_a_north:"Taxiway A · N",fbo_apron_detour:"Taxiway C"};
  const runwayShapes = (geometry.features.runways || []).map(runway => {
    const theta = Number(runway.true_heading_degrees_from_first_threshold || 0) * Math.PI / 180;
    const half = Number(runway.length_m || 0) / 2, width = Number(runway.width_m || 25) / 2;
    const ux = Math.sin(theta), uy = Math.cos(theta), px = Math.cos(theta), py = -Math.sin(theta);
    const x = Number(runway.center_local_m.x), y = Number(runway.center_local_m.y);
    const corners = [[x-ux*half-px*width,y-uy*half-py*width],[x+ux*half-px*width,y+uy*half-py*width],[x+ux*half+px*width,y+uy*half+py*width],[x-ux*half+px*width,y-uy*half+py*width]].map(([a,b])=>p.point(a,b).join(",")).join(" ");
    return `<polygon points="${corners}" fill="#6b777d" stroke="#5b686e" stroke-width="1"/><text x="${p.point(x,y)[0]}" y="${p.point(x,y)[1]+3}" text-anchor="middle" class="runway-label">${esc(runway.identifier)}</text>`;
  }).join("");
  const features = geometry.features || {};
  let apron = "";
  if (features.apron_center_local_m && features.apron_size_m) {
    const a = p.point(features.apron_center_local_m.x, features.apron_center_local_m.y), w = features.apron_size_m.x*p.scale, h=features.apron_size_m.y*p.scale;
    apron = `<rect x="${a[0]-w/2}" y="${a[1]-h/2}" width="${w}" height="${h}" fill="#cbd8ce" stroke="#b9c9bf" stroke-width="1" rx="6"/>`;
  }
  const buildings = (features.buildings || []).map(b=>{const c=p.point(b.center_local_m.x,b.center_local_m.y),w=b.size_m.x*p.scale,h=b.size_m.y*p.scale;return `<rect x="${c[0]-w/2}" y="${c[1]-h/2}" width="${w}" height="${h}" fill="#d4d7d2" stroke="#bfc7c3" stroke-width="1" rx="3"/>`;}).join("");
  const edges = geometry.edges.map(edge => {
    const a=nodes.get(edge.from),b=nodes.get(edge.to); if(!a||!b)return "";
    const [x1,y1]=p.point(a.x_m,a.y_m),[x2,y2]=p.point(b.x_m,b.y_m), isClosed=closed.has(edge.id), selected=state.selectedEdge===edge.id, mapped=!!mappedRouteForEdge(edge.id);
    return `<g class="edge-hit ${mapped?"edge-supported":""}" data-edge="${esc(edge.id)}" role="button" tabindex="0" aria-label="${mapped?"Select supported taxiway edge":"Inspect unsupported taxiway edge"} ${esc(a.name)} to ${esc(b.name)}"><line x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}" class="edge-outline"/><line x1="${x1}" y1="${y1}" x2="${x2}" y2="${y2}" class="edge-line ${mapped?"supported":""} ${isClosed?"closed":""} ${selected?"selected":""}"/><circle cx="${(x1+x2)/2}" cy="${(y1+y2)/2}" r="${selected?10:8}" class="edge-target"/></g>`;
  }).join("");
  const nodeMarkers = geometry.nodes.map(node=>{
    const [x,y]=p.point(node.x_m,node.y_m), isGate=geometry.gates.some(g=>g.node===node.id);
    return `<g class="node-marker ${isGate?"gate-marker":""}" ${isGate?`aria-label="Stand ${esc(geometry.gates.find(g=>g.node===node.id).id)}"`:""}><circle cx="${x}" cy="${y}" r="${isGate?7:4}"/><title>${esc(node.name)}</title>${nodeLabels[node.id]?`<text x="${x+8}" y="${y-7}" class="node-label">${esc(nodeLabels[node.id])}</text>`:""}</g>`;
  }).join("");
  const gates = geometry.gates.map((g,index)=>{const n=nodes.get(g.node);if(!n)return "";const [x,y]=p.point(n.x_m,n.y_m);return `<g class="gate-tag" aria-label="Stand ${esc(g.id)}"><rect x="${x-10}" y="${y-22}" width="20" height="15" rx="3"/><text x="${x}" y="${y-11}" text-anchor="middle">S${index+1}</text><title>${esc(g.id)}</title></g>`;}).join("");
  const vehicles=all("equipment").filter(item=>state.boot.capabilities.vehicle_types[item.type]&&item.initial_location).map(item=>{const node=nodes.get(item.initial_location);if(!node)return "";const [x,y]=p.point(node.x_m,node.y_m),out=closedEquipment().has(item.equipment_id),selected=state.selectedVehicle===item.equipment_id;return `<g class="vehicle-marker ${out?"unavailable":""} ${selected?"selected":""}" data-vehicle="${esc(item.equipment_id)}" role="button" tabindex="0" aria-label="${out?"Unavailable":"Available"} ${esc(item.type.replaceAll("_"," "))} vehicle ${esc(item.equipment_id)}"><circle cx="${x}" cy="${y}" r="${selected?11:9}"/><text x="${x}" y="${y+3}" text-anchor="middle">${out?"!":"V"}</text><title>${esc(item.equipment_id)} · ${esc(item.type)} · ${out?"unavailable":"available"}</title></g>`;}).join("");
  const north = `<g transform="translate(48 48)"><text x="0" y="0" class="north-mark">N</text><path d="M0 8 L-7 29 L0 25 L7 29 Z" fill="#536873"/></g>`;
  $("#airport-map").innerHTML = `<svg viewBox="0 0 ${p.width} ${p.height}" aria-label="Operational airport diagram"><defs><pattern id="map-grid" width="34" height="34" patternUnits="userSpaceOnUse"><path d="M34 0H0V34" fill="none" stroke="#dde5e1" stroke-width=".8"/></pattern></defs><rect width="1000" height="640" fill="#eef2ed"/><rect width="1000" height="640" fill="url(#map-grid)"/>${north}${apron}${buildings}${edges}${runwayShapes}${nodeMarkers}${gates}${vehicles}<g transform="translate(38 590)"><path d="M0 0H150M0 -5V5M75 -4V4M150 -5V5" stroke="#596c74" stroke-width="2"/><text x="0" y="19" class="scale-label">0</text><text x="65" y="19" class="scale-label">500 m</text><text x="139" y="19" class="scale-label">1 km</text></g></svg>`;
  $$(".edge-hit", $("#airport-map")).forEach(el=>{
    el.addEventListener("click",()=>selectEdge(el.dataset.edge));
    el.addEventListener("keydown",event=>{if(event.key==="Enter"||event.key===" "){event.preventDefault();selectEdge(el.dataset.edge);}});
  });
  $$(".vehicle-marker",$("#airport-map")).forEach(el=>{el.addEventListener("click",()=>selectVehicle(el.dataset.vehicle));el.addEventListener("keydown",event=>{if(event.key==="Enter"||event.key===" "){event.preventDefault();selectVehicle(el.dataset.vehicle);}});});
}

function closedEquipment(){return new Set(activeDisruptions().filter(item=>item.type==="equipment_outage").map(item=>item.equipment_id));}

function pair(label, value) { return `<div class="inspection-pair"><span>${esc(label)}</span><strong>${esc(value ?? "—")}</strong></div>`; }
function renderInspector() {
  const body=$("#inspector-body");
  if(state.selectedVehicle){
    const item=all("equipment").find(row=>row.equipment_id===state.selectedVehicle);if(!item){state.selectedVehicle=null;return renderInspector();}
    const offline=closedEquipment().has(item.equipment_id);
    $("#inspector-title").textContent="Ground vehicle";$("#inspector-type").textContent=offline?"Outage planned":"Vehicle";
    body.innerHTML=`<p class="inspector-subtitle">${esc(item.equipment_id)}</p>${pair("Equipment type",String(item.type).replaceAll("_"," "))}${pair("Availability",offline?"Unavailable during this run":item.status||"Available")}${pair("Initial location",item.initial_location||"Not mapped")}${pair("Simulator support","Mapped in this dataset")}<div class="support-note">Vehicle outage changes are applied from the selected simulation time until the run ends. Timed vehicle recovery is not supported.</div>${!offline?`<button class="button primary" id="inspect-add-outage" style="width:100%">＋ Plan vehicle outage</button>`:""}<button class="text-button" id="clear-selection" style="margin-top:12px">Clear selection</button>`;
    $("#inspect-add-outage")?.addEventListener("click",()=>showDisruptionForm("equipment_outage",item.equipment_id));$("#clear-selection")?.addEventListener("click",()=>{state.selectedVehicle=null;render();});return;
  }
  if (!state.selectedEdge) {
    $("#inspector-title").textContent="Airport details"; $("#inspector-type").textContent=state.boot.airport.id;
    const a=state.boot.airport, g=state.boot.geometry;
    const nativeCatalog=Object.entries(state.boot.catalog.scenarios||{}).map(([key,item])=>`<div class="catalog-item"><strong>${esc(key.replaceAll("-"," "))}</strong><small>${esc(item.scenario)} · ${esc(item.description)}</small></div>`).join("");
    body.innerHTML=`<p class="inspector-subtitle">${esc(a.name)}</p>${pair("Airport identifier",a.id)}${pair("Dataset",a.dataset_id)}${pair("Flights",all("flights").length)}${pair("Aircraft",all("aircraft").length)}${pair("Vehicles",all("equipment").length)}${pair("Stands",all("gates").length)}<div class="inspector-separator"></div><p class="support-note"><strong>Data provenance</strong><br>${esc(a.dataset_description || "Source provenance is included with this airport package.")}</p><p class="field-hint">${esc(g.features?.geometry_status || "Airport geometry follows the selected dataset.")}</p><details class="catalog-reference"><summary>Goal 23 native scenario catalog</summary><p class="field-hint">Reference scenarios use their native release YAML and mappings; they are not editable as this airport package.</p>${nativeCatalog}</details>`;
    return;
  }
  const edge=state.boot.geometry.edges.find(item=>item.id===state.selectedEdge); if(!edge)return;
  const a=state.boot.geometry.nodes.find(item=>item.id===edge.from), b=state.boot.geometry.nodes.find(item=>item.id===edge.to), resource=mappedRouteForEdge(edge.id);
  const closure=resource&&activeDisruptions().find(item=>item.type==="route_closure"&&item.resource_id===resource);
  $("#inspector-title").textContent=resource?"Taxiway segment":"Taxiway route"; $("#inspector-type").textContent=resource?"Closure supported":"View only";
  body.innerHTML=`<div class="field"><label>Segment</label><input readonly value="${esc(a?.name||edge.from)} → ${esc(b?.name||edge.to)}"></div>${pair("Route length",`${Number(edge.distance_m).toFixed(0)} m`)}${pair("Travel estimate",`${edge.traversal_time_seconds} sec`)}${pair("Connected points",`${edge.from} / ${edge.to}`)}${resource?`<div class="inspector-separator"></div><p class="field-hint">This segment is mapped to an editable RampLab route in the selected dataset.</p>${closure?`<div class="notice warning"><strong>Closure scheduled</strong><br>${esc(niceTime(closure.start_time))}${closure.end_time?` → ${esc(niceTime(closure.end_time))}`:" · through the run"}</div><button class="button outline" id="review-closure" style="width:100%;margin-top:8px">Review disruption</button>`:`<button class="button primary" id="add-closure" style="width:100%;margin-top:12px">＋ Add route closure</button>`}`:`<div class="support-note"><strong>Map inspection only</strong><br>This edge has no scenario-generator route mapping, so it cannot be used for a simulation closure.</div>`}<button class="text-button" id="clear-selection" style="margin-top:12px">Clear selection</button>`;
  $("#add-closure")?.addEventListener("click",()=>addClosure(resource)); $("#review-closure")?.addEventListener("click",()=>nav("disruptions")); $("#clear-selection")?.addEventListener("click",()=>{state.selectedEdge=null;render();});
}

function selectEdge(edgeId) { state.selectedEdge=edgeId;state.selectedVehicle=null; state.view="airport"; render(); }
function selectVehicle(id){state.selectedVehicle=id;state.selectedEdge=null;state.view="airport";render();}
function defaultWindow() { return {start:"2026-10-09T14:00:15Z",end:"2026-10-09T14:45:00Z"}; }
function addClosure(resourceId) {
  if (!resourceId) return;
  if(activeDisruptions().some(item=>item.type==="route_closure"&&item.resource_id===resourceId)){toast("A closure for this taxiway is already in the plan.");return;}
  const time=defaultWindow(); markEdited();
  state.scenario.disruptions.push({disruption_id:`STUDIO-CLOSURE-${Date.now()}`,type:"route_closure",resource_id:resourceId,start_time:time.start,end_time:time.end,reason:"Scenario Studio route closure",description:"Selected visually on the airport map",enabled:true});
  render(); toast("Route closure added to the variant.");
}

function nav(view) { state.view=view; render(); }
function niceTime(value) { if(!value)return "—";const date=new Date(value);return Number.isNaN(+date)?String(value):new Intl.DateTimeFormat(undefined,{dateStyle:"medium",timeStyle:"short"}).format(date); }
function dateInput(value) { const date=new Date(value);if(Number.isNaN(+date))return "";const pad=n=>String(n).padStart(2,"0");return `${date.getFullYear()}-${pad(date.getMonth()+1)}-${pad(date.getDate())}T${pad(date.getHours())}:${pad(date.getMinutes())}`; }
function fromDateInput(value) { const date=new Date(value);return Number.isNaN(+date)?"":date.toISOString(); }
function parseLocalStamp(value) { return fromDateInput(value); }

function renderFlights() {
  const rows=all("flights"), aircraft=all("aircraft"), gates=all("gates");
  const display=rows.map((row,index)=>({row,index})).sort((a,b)=>state.flightSort==="flight"?String(a.row.flight_id).localeCompare(String(b.row.flight_id)):String(a.row.scheduled_time).localeCompare(String(b.row.scheduled_time)));
  $("#view-flights").innerHTML=`<div class="summary-grid"><div class="summary-cell"><strong>${rows.length}</strong><span>Scheduled operations</span></div><div class="summary-cell"><strong>${new Set(rows.map(x=>x.aircraft_id)).size}</strong><span>Aircraft referenced</span></div><div class="summary-cell"><strong>${new Set(rows.map(x=>x.gate_id)).size}</strong><span>Stands assigned</span></div><div class="summary-cell"><strong>${esc(state.boot.airport.timezone||"UTC")}</strong><span>Airport local time</span></div></div><div class="panel"><div class="panel-title"><div><h2>Flight schedule</h2><p>Times display in your local timezone and save as UTC instants.</p></div><div class="flight-tools"><input id="flight-search" type="search" placeholder="Filter flight, aircraft or stand" value="${esc(state.flightFilter)}" aria-label="Filter flights"><select id="flight-sort" aria-label="Sort flights"><option value="time" ${state.flightSort==="time"?"selected":""}>Sort by time</option><option value="flight" ${state.flightSort==="flight"?"selected":""}>Sort by flight</option></select></div></div><div class="table-wrap"><table class="studio-table"><thead><tr><th>Flight</th><th>Aircraft</th><th>Operation</th><th>Scheduled local time</th><th>Stand</th><th>Source</th></tr></thead><tbody>${display.map(({row,i:index})=>`<tr data-filter="${esc([row.flight_id,row.flight_number,row.aircraft_id,row.gate_id,row.operation].join(" ").toLowerCase())}"><td><input class="table-input flight-field" data-index="${index}" data-key="flight_number" value="${esc(row.flight_number||row.flight_id)}" aria-label="Flight identifier"></td><td><select class="table-select flight-field" data-index="${index}" data-key="aircraft_id" aria-label="Aircraft">${aircraft.map(a=>`<option value="${esc(a.aircraft_id)}" ${a.aircraft_id===row.aircraft_id?"selected":""}>${esc(a.aircraft_id)}</option>`).join("")}</select></td><td><select class="table-select flight-field" data-index="${index}" data-key="operation" aria-label="Operation"><option value="arrival" ${row.operation==="arrival"?"selected":""}>Arrival</option><option value="departure" ${row.operation==="departure"?"selected":""}>Departure</option></select></td><td><input type="datetime-local" class="table-input flight-field" data-index="${index}" data-key="scheduled_time" value="${dateInput(row.scheduled_time)}" aria-label="Scheduled local time"></td><td><select class="table-select flight-field" data-index="${index}" data-key="gate_id" aria-label="Assigned stand">${gates.map(g=>`<option value="${esc(g.gate_id)}" ${g.gate_id===row.gate_id?"selected":""}>${esc(g.gate_id)}</option>`).join("")}</select></td><td><span class="badge">${row.source?esc(row.source):"Scenario input"}</span></td></tr>`).join("")}</tbody></table></div><p class="field-hint" style="margin-top:12px">Aircraft type and initial aircraft state are informational for this generator. Flight timing, aircraft association, operation and stand are applied to generated simulator inputs.</p></div>`;
  $("#flight-search").addEventListener("input",event=>{state.flightFilter=event.target.value.toLowerCase();$$("tbody tr",$("#view-flights")).forEach(row=>row.hidden=!row.dataset.filter.includes(state.flightFilter));});
  $("#flight-sort").addEventListener("change",event=>{state.flightSort=event.target.value;renderFlights();});
  $$(".flight-field",$("#view-flights")).forEach(input=>input.addEventListener("change",()=>{
    const row=all("flights")[Number(input.dataset.index)],key=input.dataset.key,previousId=row.flight_id;let value=input.value;
    if(key==="scheduled_time")value=parseLocalStamp(value);
    if(key==="flight_number"){row.flight_number=value;row.flight_id=value.startsWith(`${state.boot.airport.id}-`)?value:`${state.boot.airport.id}-${value}`;}
    else row[key]=value;
    if(key==="flight_number")for(const disruption of all("disruptions"))if(disruption.flight_id===previousId)disruption.flight_id=row.flight_id;
    markEdited();render();
  }));
}

function renderFleet() {
  const vehicles=all("equipment"), capabilities=state.boot.capabilities.vehicle_types, nodes=state.boot.geometry.nodes;
  const counts={};for(const v of vehicles)counts[v.type]=(counts[v.type]||0)+1;
  const countText=Object.entries(counts).map(([type,count])=>`${esc(type.replaceAll("_"," "))}: ${count}`).join(" · ");
  $("#view-fleet").innerHTML=vehicles.length?`<div class="summary-grid"><div class="summary-cell"><strong>${vehicles.length}</strong><span>Listed vehicles</span></div><div class="summary-cell"><strong>${Object.keys(counts).length}</strong><span>Equipment types</span></div><div class="summary-cell"><strong>${vehicles.filter(v=>v.status==="available").length}</strong><span>Available</span></div><div class="summary-cell"><strong>${Object.keys(capabilities).length}</strong><span>Mapped simulator types</span></div></div><div class="panel"><div class="panel-title"><div><h2>Mobile fleet</h2><p>${countText||"Current scenario equipment"}</p></div></div><div class="table-wrap"><table class="studio-table"><thead><tr><th>Vehicle</th><th>Type</th><th>Availability</th><th>Initial location</th><th>Simulator mapping</th></tr></thead><tbody>${vehicles.map((v,i)=>{const supported=Object.hasOwn(capabilities,v.type);return `<tr><td class="flight-id">${esc(v.equipment_id)}</td><td>${supported?`<select class="table-select fleet-field" data-index="${i}" data-key="type" aria-label="Vehicle type">${Object.keys(capabilities).map(type=>`<option ${type===v.type?"selected":""} value="${esc(type)}">${esc(type.replaceAll("_"," "))}</option>`).join("")}</select>`:`<span class="readonly-value">${esc(v.type.replaceAll("_"," "))}</span>`}</td><td>${supported?`<select class="table-select fleet-field" data-index="${i}" data-key="status" aria-label="Vehicle availability"><option value="available" ${v.status==="available"?"selected":""}>Available</option><option value="unavailable" ${v.status!=="available"?"selected":""}>Unavailable</option></select>`:`<span class="readonly-value">Metadata only</span>`}</td><td>${supported?`<select class="table-select fleet-field" data-index="${i}" data-key="initial_location" aria-label="Initial location">${nodes.map(n=>`<option value="${esc(n.id)}" ${n.id===(v.initial_location||"")?"selected":""}>${esc(n.name)}</option>`).join("")}</select>`:`<span class="readonly-value">${esc(v.initial_location||"Not mapped")}</span>`}</td><td><span class="badge ${supported?"green":"orange"}">${supported?"Mapped":"Not mapped"}</span></td></tr>`;}).join("")}</tbody></table></div><p class="field-hint" style="margin-top:12px">Mapped vehicle type, availability and initial location are included in Goal 24B input. Unmapped equipment is metadata only. Vehicle speed and service assignment are read-only because the selected generator mapping does not expose them.</p></div>`:`<div class="panel"><div class="empty-state"><span class="empty-symbol">▣</span><h3>No modeled ground vehicles</h3><p>${esc(state.boot.airport.id)}'s selected package contains no equipment inventory. The public airport source does not provide an equipment list, so the Studio will not add assumed vehicles or offer an outage control for this dataset.</p><p style="margin-top:12px">Try the <button class="text-button" data-switch-airport="kauo-goal27">two-aircraft KAUO prototype</button> or <button class="text-button" data-switch-airport="synthetic">synthetic example</button> to inspect other generator inputs.</p></div></div>`;
  $$(".fleet-field",$("#view-fleet")).forEach(input=>input.addEventListener("change",()=>{all("equipment")[Number(input.dataset.index)][input.dataset.key]=input.value;markEdited();render();}));
  $$('[data-switch-airport]',$("#view-fleet")).forEach(button=>button.addEventListener("click",()=>loadAirport(button.dataset.switchAirport)));
}

function renderTurnarounds() {
  const tasks=all("turnaround_requirements"),aircraft=all("aircraft");
  $("#view-turnarounds").innerHTML=tasks.length?`<div class="panel"><div class="panel-title"><div><h2>Service requirements</h2><p>Only the listed service duration and requirement links are consumed by this generator.</p></div><span class="badge">${tasks.length} services</span></div><div class="table-wrap"><table class="studio-table"><thead><tr><th>Service</th><th>Aircraft</th><th>Service type</th><th>Duration</th><th>Dependency</th><th>Support</th></tr></thead><tbody>${tasks.map((task,i)=>`<tr><td class="flight-id">${esc(task.requirement_id)}</td><td><select class="table-select task-field" data-index="${i}" data-key="aircraft_id" aria-label="Service aircraft">${aircraft.map(a=>`<option value="${esc(a.aircraft_id)}" ${a.aircraft_id===task.aircraft_id?"selected":""}>${esc(a.aircraft_id)}</option>`).join("")}</select></td><td><span class="readonly-value">${esc(String(task.service_type||"").replaceAll("_"," "))}</span></td><td><input class="table-input task-field" type="number" min="0.01" step="0.1" data-index="${i}" data-key="duration_minutes" value="${esc(task.duration_minutes)}" aria-label="Service duration in minutes" style="max-width:110px"> <span class="muted">min</span></td><td><span class="readonly-value">${esc(task.depends_on||task.dependency_requirement_id||"None")}</span></td><td><span class="badge green">Supported</span></td></tr>`).join("")}</tbody></table></div><div class="notice" style="margin-top:14px">The current KAUO package models an abstract pushback preparation task as a scenario assumption. It does not claim an observed service, actual equipment inventory, or measured turnaround duration.</div></div>`:`<div class="panel"><div class="empty-state"><span class="empty-symbol">↻</span><h3>No turnaround services in this package</h3><p>The selected canonical dataset has no service requirements. The Studio only edits services already represented by the Goal 24A package and supported by Goal 24B.</p></div></div>`;
  $$(".task-field",$("#view-turnarounds")).forEach(input=>input.addEventListener("change",()=>{const row=all("turnaround_requirements")[Number(input.dataset.index)];row[input.dataset.key]=input.dataset.key==="duration_minutes"?Number(input.value):input.value;markEdited();render();}));
}

function renderEnvironment() {
  const flights=all("flights"),ordered=[...flights].sort((a,b)=>String(a.scheduled_time).localeCompare(String(b.scheduled_time)));
  const runways=state.boot.airport.runways||[];
  $("#view-environment").innerHTML=`<div class="split-panels"><div class="panel"><div class="panel-title"><div><h2>Simulation settings</h2><p>Only settings consumed by the current generator are editable.</p></div><span class="badge">Goal 24B</span></div><div class="field" style="max-width:260px"><label for="scenario-seed">Random seed</label><input id="scenario-seed" type="number" min="0" max="2147483647" step="1" value="${esc(state.scenario.seed)}"><span class="field-hint">The seed is written into generated RampLab scenario data and controls deterministic simulator initialization.</span></div><div class="inspector-separator"></div>${pair("Airport local timezone",state.boot.airport.timezone||"Not supplied")}${pair("Scheduled window start",ordered.length?niceTime(ordered[0].scheduled_time):"No flights")}${pair("Scheduled window end",ordered.length?niceTime(ordered[ordered.length-1].scheduled_time):"No flights")}${pair("Flight operations",flights.length)}</div><div class="panel"><div class="panel-title"><div><h2>Airport environment</h2><p>Source and modeled conditions for this experiment.</p></div></div>${runways.map(runway=>`<div class="inspection-pair"><span>Runway ${esc(runway.identifier)}</span><strong>${esc(runway.length_ft)} ft · ${esc(runway.surface)}</strong></div>`).join("")}<div class="notice warning" style="margin-top:14px"><strong>Weather is not modeled</strong><br>Weather selectors are unavailable because the current RampLab simulator does not apply weather assumptions.</div><p class="field-hint" style="margin-top:12px">Scenario start time is derived from the scheduled flight operations and active disruptions. It is not an independent simulator control.</p></div></div>`;
  $("#scenario-seed").addEventListener("change",event=>{const value=Number(event.target.value);if(!Number.isInteger(value)||value<0||value>2147483647){toast("Seed must be a whole number from 0 through 2,147,483,647.",true);event.target.value=state.scenario.seed;return;}state.scenario.seed=value;markEdited();render();});
}

function renderDisruptions() {
  const items=all("disruptions"),types=state.boot.capabilities;
  const availableVehicles=all("equipment").filter(item=>types.vehicle_types[item.type]&&item.status==="available");
  $("#view-disruptions").innerHTML=`<div class="panel"><div class="panel-title"><div><h2>Active scenario changes</h2><p>Controls appear only for disruption types the selected Goal 24B mapping can generate.</p></div><div class="row-actions"><button class="button outline" id="add-disruption">＋ Route closure</button>${availableVehicles.length?`<button class="button outline" id="add-outage">＋ Vehicle outage</button>`:""}${all("flights").length?`<button class="button outline" id="add-flight-delay">＋ Flight delay</button>`:""}${all("turnaround_requirements").length?`<button class="button outline" id="add-service-delay">＋ Service delay</button>`:""}</div></div>${items.length?items.map((item,index)=>{
    const target=item.type==="route_closure"?item.resource_id:item.equipment_id||item.flight_id||item.requirement_id;
    const supported=item.type==="route_closure"?Boolean(types.mapped_routes[item.resource_id]):item.type==="equipment_outage"?all("equipment").some(row=>row.equipment_id===item.equipment_id)&&!item.end_time&&!item.duration_minutes:["flight_delay","delayed_service"].includes(item.type);
    const label=({route_closure:"Taxiway closure",equipment_outage:"Vehicle outage",flight_delay:"Flight delay",delayed_service:"Service delay"})[item.type]||String(item.type).replaceAll("_"," ");
    const mapEdge=item.type==="route_closure"?types.mapped_routes[item.resource_id]:null;
    return `<div class="disruption-row"><div class="row-main"><strong>${esc(label)} · ${esc(target||"No target")}</strong><small>${esc(niceTime(item.start_time))}${item.end_time?` → ${esc(niceTime(item.end_time))}`:" · rest of simulation"} · ${supported?"Generator-supported":"Not supported for this target"}</small>${!supported?`<small class="impact-regression">Disabled until the target has a valid generator mapping.</small>`:""}</div><div class="row-actions">${mapEdge?`<button class="text-button" data-focus-edge="${esc(mapEdge)}">Show on map</button>`:""}<button class="text-button edit-disruption" data-index="${index}">Edit</button><label title="Include in generated scenario"><input class="toggle disruption-toggle" type="checkbox" data-index="${index}" ${item.enabled!==false?"checked":""} ${supported?"":"disabled"}></label><button class="text-button danger remove-disruption" data-index="${index}">Remove</button></div></div>`;
  }).join(""):`<div class="empty-state"><span class="empty-symbol">△</span><h3>No disruptions added</h3><p>Create a variant, select a mapped taxiway route on the airport map, and add a closure. The baseline remains unchanged.</p><button class="button outline" id="add-disruption-empty">＋ Add route closure</button></div>`}<div class="notice warning" style="margin-top:14px"><strong>Availability note</strong><br>Vehicle outages last for the rest of the run. Timed vehicle recovery, stand outages and service delays without a generated task are not supported by the current adapter.</div></div><div id="disruption-form"></div>`;
  $("#add-disruption")?.addEventListener("click",()=>showDisruptionForm("route_closure"));$("#add-outage")?.addEventListener("click",()=>showDisruptionForm("equipment_outage"));$("#add-flight-delay")?.addEventListener("click",()=>showDisruptionForm("flight_delay"));$("#add-service-delay")?.addEventListener("click",()=>showDisruptionForm("delayed_service"));$("#add-disruption-empty")?.addEventListener("click",()=>showDisruptionForm("route_closure"));
  $$(".disruption-toggle").forEach(input=>input.addEventListener("change",()=>{all("disruptions")[Number(input.dataset.index)].enabled=input.checked;markEdited();render();}));
  $$(".remove-disruption").forEach(button=>button.addEventListener("click",()=>{state.scenario.disruptions.splice(Number(button.dataset.index),1);markEdited();render();}));
  $$(".edit-disruption").forEach(button=>button.addEventListener("click",()=>editDisruption(Number(button.dataset.index))));
  $$('[data-focus-edge]').forEach(button=>button.addEventListener("click",()=>selectEdge(button.dataset.focusEdge)));
}

function editDisruption(index){
  const item=all("disruptions")[index];if(!item)return;
  const type=item.type;
  if(type==="route_closure")showDisruptionForm(type,item.resource_id,item);
  else if(type==="equipment_outage")showVehicleOutageForm(item.equipment_id,item);
  else if(type==="flight_delay")showFlightDelayForm(item);
  else if(type==="delayed_service")showServiceDelayForm(item);
  else toast("This disruption type is not editable in the Studio.",true);
}

function showDisruptionForm(type="route_closure",prefillTarget=null,editing=null) {
  if(type==="equipment_outage")return showVehicleOutageForm(prefillTarget,editing);
  if(type==="flight_delay")return showFlightDelayForm(editing);
  if(type==="delayed_service")return showServiceDelayForm(editing);
  const resources=Object.entries(state.boot.capabilities.mapped_routes);
  const body=$("#disruption-form");
  if(!resources.length){toast("This dataset has no mapped route closure targets.",true);return;}
  const firstTime=all("flights").map(item=>item.scheduled_time).filter(Boolean).sort()[0]||"2026-10-09T14:00:00Z";
  const startDefault=dateInput(editing?.start_time||firstTime),endDefault=dateInput(editing?.end_time||new Date(new Date(firstTime).getTime()+45*60*1000).toISOString());
  body.innerHTML=`<div class="panel" style="margin-top:14px"><div class="panel-title"><div><h2>${editing?"Edit":"Add"} a taxiway closure</h2><p>Choose the route visually from the airport map, then set its closure window.</p></div><button id="cancel-disruption" class="icon-button" aria-label="Cancel">×</button></div><div class="split-panels"><div><div class="field"><label>Selected taxiway route</label><select id="closure-target">${resources.map(([resource,id])=>`<option value="${esc(resource)}" ${resource===(editing?.resource_id||mappedRouteForEdge(state.selectedEdge))?"selected":""}>${esc(resource.replaceAll("-"," "))}</option>`).join("")}</select><span class="field-hint">The route selector corresponds to the highlighted map edge.</span></div><button id="select-on-map" class="button outline">Select target on map</button></div><div><div class="field"><label for="closure-start">Start (airport local time)</label><input id="closure-start" type="datetime-local" value="${startDefault}"></div><div class="field"><label for="closure-end">End (airport local time)</label><input id="closure-end" type="datetime-local" value="${endDefault}"></div></div></div><div class="field"><label for="closure-reason">Reason</label><input id="closure-reason" value="${esc(editing?.reason||"Scenario planning exercise")}" maxlength="160"></div><div class="dialog-actions"><button id="cancel-disruption-bottom" class="button outline">Cancel</button><button id="save-disruption" class="button primary">${editing?"Save closure":"Add closure"}</button></div></div>`;
  const select=$("#closure-target");if(prefillTarget)select.value=prefillTarget;
  $("#select-on-map").addEventListener("click",()=>{body.innerHTML=`<div class="notice">Select one of the highlighted supported taxiway edges on the airport map. <button class="text-button" id="return-disruption-form">Return to closure details</button></div>`;$("#return-disruption-form").addEventListener("click",showDisruptionForm);nav("airport");});
  $("#cancel-disruption").addEventListener("click",()=>body.innerHTML=""); $("#cancel-disruption-bottom").addEventListener("click",()=>body.innerHTML="");
  $("#save-disruption").addEventListener("click",()=>{
    const start=parseLocalStamp($("#closure-start").value),end=parseLocalStamp($("#closure-end").value);
    if(!start||!end||new Date(end)<=new Date(start)){toast("Choose an end time after the closure starts.",true);return;}
    const target=select.value;if(activeDisruptions().some(item=>item!==editing&&item.type==="route_closure"&&item.resource_id===target)){toast("A closure for this taxiway is already active.",true);return;}
    const reason=$("#closure-reason").value.trim();markEdited();if(editing){Object.assign(editing,{resource_id:target,start_time:start,end_time:end,reason,description:reason});}else state.scenario.disruptions.push({disruption_id:`STUDIO-CLOSURE-${Date.now()}`,type:"route_closure",resource_id:target,start_time:start,end_time:end,reason,description:reason,enabled:true});body.innerHTML="";render();toast(editing?"Taxiway closure updated.":"Taxiway closure added.");
  });
}

function showVehicleOutageForm(prefillTarget=null,editing=null){
  const vehicles=all("equipment").filter(item=>state.boot.capabilities.vehicle_types[item.type]&&item.status==="available");
  const body=$("#disruption-form");if(!vehicles.length){toast("This dataset has no available mapped vehicles.",true);return;}
  const firstTime=all("flights").map(item=>item.scheduled_time).filter(Boolean).sort()[0]||"2026-10-09T14:00:00Z";
  body.innerHTML=`<div class="panel" style="margin-top:14px"><div class="panel-title"><div><h2>${editing?"Edit":"Plan a"} vehicle outage</h2><p>The vehicle becomes unavailable from the selected time until the simulation ends.</p></div><button id="cancel-outage" class="icon-button" aria-label="Cancel">×</button></div><div class="split-panels"><div class="field"><label for="outage-target">Vehicle</label><select id="outage-target">${vehicles.map(item=>`<option value="${esc(item.equipment_id)}" ${item.equipment_id===(prefillTarget||"")?"selected":""}>${esc(item.equipment_id)} · ${esc(item.type.replaceAll("_"," "))}</option>`).join("")}</select><span class="field-hint">Mapped vehicle: select its marker on the map to identify it.</span></div><div class="field"><label for="outage-start">Outage starts (airport local time)</label><input id="outage-start" type="datetime-local" value="${dateInput(editing?.start_time||firstTime)}"></div></div><div class="field"><label for="outage-reason">Reason</label><input id="outage-reason" value="${esc(editing?.reason||"Scenario planning exercise")}" maxlength="160"></div><div class="notice warning">The current simulator does not restore a vehicle at a scheduled end time. This outage lasts for the rest of the run.</div><div class="dialog-actions"><button id="cancel-outage-bottom" class="button outline">Cancel</button><button id="save-outage" class="button primary">${editing?"Save outage":"Add vehicle outage"}</button></div></div>`;
  const cancel=()=>body.innerHTML="";$("#cancel-outage").addEventListener("click",cancel);$("#cancel-outage-bottom").addEventListener("click",cancel);
  $("#save-outage").addEventListener("click",()=>{const start=parseLocalStamp($("#outage-start").value);if(!start){toast("Choose a valid outage start time.",true);return;}const target=$("#outage-target").value;if(activeDisruptions().some(item=>item!==editing&&item.type==="equipment_outage"&&item.equipment_id===target)){toast("An outage for this vehicle already exists.",true);return;}const reason=$("#outage-reason").value.trim();markEdited();if(editing)Object.assign(editing,{equipment_id:target,start_time:start,reason,description:reason});else state.scenario.disruptions.push({disruption_id:`STUDIO-OUTAGE-${Date.now()}`,type:"equipment_outage",equipment_id:target,start_time:start,reason,description:reason,enabled:true});body.innerHTML="";render();toast(editing?"Vehicle outage updated.":"Vehicle outage added. It lasts through the rest of this run.");});
}

function showFlightDelayForm(editing=null){
  const flights=all("flights"),body=$("#disruption-form");if(!flights.length){toast("There are no flights to delay.",true);return;}
  body.innerHTML=`<div class="panel" style="margin-top:14px"><div class="panel-title"><div><h2>${editing?"Edit":"Plan a"} flight delay</h2><p>RampLab applies this delay to the selected flight's generated operation time.</p></div><button id="cancel-delay" class="icon-button" aria-label="Cancel">×</button></div><div class="split-panels"><div class="field"><label for="delay-flight">Flight</label><select id="delay-flight">${flights.map(item=>`<option value="${esc(item.flight_id)}" ${item.flight_id===editing?.flight_id?"selected":""}>${esc(item.flight_number||item.flight_id)} · ${esc(item.operation)}</option>`).join("")}</select></div><div class="field"><label for="delay-start">Delay applies at (airport local time)</label><input id="delay-start" type="datetime-local" value="${dateInput(editing?.start_time||flights[0].scheduled_time)}"></div></div><div class="field"><label for="delay-minutes">Delay amount (minutes)</label><input id="delay-minutes" type="number" min="0.1" step="1" value="${esc(editing?.delay_minutes||15)}"></div><div class="field"><label for="delay-reason">Reason</label><input id="delay-reason" value="${esc(editing?.reason||"Scenario planning exercise")}" maxlength="160"></div><div class="dialog-actions"><button id="cancel-delay-bottom" class="button outline">Cancel</button><button id="save-delay" class="button primary">${editing?"Save delay":"Add flight delay"}</button></div></div>`;
  const cancel=()=>body.innerHTML="";$("#cancel-delay").addEventListener("click",cancel);$("#cancel-delay-bottom").addEventListener("click",cancel);
  $("#save-delay").addEventListener("click",()=>{const start=parseLocalStamp($("#delay-start").value),amount=Number($("#delay-minutes").value);if(!start||!Number.isFinite(amount)||amount<=0){toast("Choose a valid start time and a delay greater than zero minutes.",true);return;}const target=$("#delay-flight").value,reason=$("#delay-reason").value.trim();markEdited();if(editing)Object.assign(editing,{flight_id:target,start_time:start,delay_minutes:amount,reason,description:reason});else state.scenario.disruptions.push({disruption_id:`STUDIO-FLIGHT-DELAY-${Date.now()}`,type:"flight_delay",flight_id:target,start_time:start,delay_minutes:amount,reason,description:reason,enabled:true});body.innerHTML="";render();toast(editing?"Flight delay updated.":"Flight delay added.");});
}

function showServiceDelayForm(editing=null){
  const requirements=all("turnaround_requirements"),body=$("#disruption-form");if(!requirements.length){toast("There are no turnaround services to delay.",true);return;}
  const firstTime=all("flights").map(item=>item.scheduled_time).filter(Boolean).sort()[0]||"2026-10-09T14:00:00Z";
  body.innerHTML=`<div class="panel" style="margin-top:14px"><div class="panel-title"><div><h2>${editing?"Edit":"Plan a"} service delay</h2><p>This increases the selected generated service task duration.</p></div><button id="cancel-service-delay" class="icon-button" aria-label="Cancel">×</button></div><div class="split-panels"><div class="field"><label for="delay-service">Turnaround service</label><select id="delay-service">${requirements.map(item=>`<option value="${esc(item.requirement_id)}" ${item.requirement_id===editing?.requirement_id?"selected":""}>${esc(item.requirement_id)} · ${esc(String(item.service_type).replaceAll("_"," "))}</option>`).join("")}</select></div><div class="field"><label for="service-delay-start">Delay applies at (airport local time)</label><input id="service-delay-start" type="datetime-local" value="${dateInput(editing?.start_time||firstTime)}"></div></div><div class="field"><label for="service-delay-minutes">Additional duration (minutes)</label><input id="service-delay-minutes" type="number" min="0.1" step="1" value="${esc(editing?.delay_minutes||10)}"></div><div class="field"><label for="service-delay-reason">Reason</label><input id="service-delay-reason" value="${esc(editing?.reason||"Scenario planning exercise")}" maxlength="160"></div><div class="dialog-actions"><button id="cancel-service-delay-bottom" class="button outline">Cancel</button><button id="save-service-delay" class="button primary">${editing?"Save delay":"Add service delay"}</button></div></div>`;
  const cancel=()=>body.innerHTML="";$("#cancel-service-delay").addEventListener("click",cancel);$("#cancel-service-delay-bottom").addEventListener("click",cancel);
  $("#save-service-delay").addEventListener("click",()=>{const start=parseLocalStamp($("#service-delay-start").value),amount=Number($("#service-delay-minutes").value);if(!start||!Number.isFinite(amount)||amount<=0){toast("Choose a valid start time and an added duration greater than zero minutes.",true);return;}const target=$("#delay-service").value,reason=$("#service-delay-reason").value.trim();markEdited();if(editing)Object.assign(editing,{requirement_id:target,start_time:start,delay_minutes:amount,reason,description:reason});else state.scenario.disruptions.push({disruption_id:`STUDIO-SERVICE-DELAY-${Date.now()}`,type:"delayed_service",requirement_id:target,start_time:start,delay_minutes:amount,reason,description:reason,enabled:true});body.innerHTML="";render();toast(editing?"Service delay updated.":"Service delay added.");});
}

async function validateCurrent(show=true) {
  try {
    const result=await api("/api/studio/validate",{scenario:state.scenario});
    state.validation=result;
    if(show){nav("validation");toast(result.valid?"Scenario is ready to run.":"Resolve the validation errors before running.",!result.valid);}
    return result;
  }catch(error){toast(error.message,true);return null;}
}

function renderValidation() {
  const validation=state.validation;
  const findings=validation?.findings||[];
  const changes=validation?.changes||[];
  $("#view-validation").innerHTML=`<div class="split-panels"><div class="panel"><div class="panel-title"><div><h2>Scenario validation</h2><p>Goal 24A relationships and Goal 24B simulator mappings</p></div>${validation?`<span class="badge ${validation.valid?"green":"orange"}">${validation.valid?"Ready to run":"Needs attention"}</span>`:`<button id="run-validation" class="button outline">Run validation</button>`}</div>${validation?`<div class="findings">${findings.length?findings.map(item=>`<div class="finding ${esc(item.severity)}"><span class="finding-symbol">${item.severity==="error"?"×":item.severity==="warning"?"!":"i"}</span><div><strong>${esc(item.severity.toUpperCase())} · ${esc(item.code.replaceAll("_"," "))}</strong><p>${esc(item.message)}</p></div></div>`).join(""):`<div class="finding info"><span class="finding-symbol">✓</span><div><strong>No validation findings</strong><p>RampLab's scenario generator accepts these inputs.</p></div></div>`}</div><button id="rerun-validation" class="button outline">Validate again</button>`:`<div class="empty-state"><span class="empty-symbol">✓</span><h3>Check this scenario before running</h3><p>The Studio uses the Goal 24A canonical validator and Goal 24B generator's package checks.</p><button id="run-validation-empty" class="button primary" style="margin-top:12px">Validate scenario</button></div>`}</div><div class="panel"><div class="panel-title"><div><h2>Changes from baseline</h2><p>Review every edit before generating simulator inputs.</p></div><span class="badge">${changes.length} ${changes.length===1?"change":"changes"}</span></div>${changes.length?changes.map(change=>`<div class="change-line"><span class="change-area">${esc(change.area)}</span><strong>${esc(change.label)}</strong>${change.before!==undefined?`<span class="change-arrow">${esc(change.before)} → ${esc(change.after)}</span>`:""}${change.target?`<span class="change-arrow">${esc(change.target)}</span>`:""}</div>`).join(""):`<div class="notice">This scenario matches its baseline. Create a variant to make operational changes.</div>`}<div class="inspector-separator"></div><div class="inspection-pair"><span>Airport dataset</span><strong>${esc(state.scenario.dataset_id)}</strong></div><div class="inspection-pair"><span>Seed</span><strong>${esc(state.scenario.seed)}</strong></div><div class="inspection-pair"><span>Active disruptions</span><strong>${activeDisruptions().length}</strong></div><button id="validate-run" class="button primary" style="margin-top:13px" ${validation?.valid===false?"disabled":""}>▶ Run this scenario</button></div></div>`;
  const advanced=document.createElement("details");advanced.className="advanced-details";advanced.innerHTML=`<summary>Advanced · inspect the editable configuration</summary><pre>${esc(JSON.stringify(state.scenario,null,2))}</pre><button id="download-generated" class="button outline">Export generated RampLab files</button>`;$("#view-validation").append(advanced);$("#download-generated").addEventListener("click",exportScenario);
  $("#run-validation")?.addEventListener("click",()=>validateCurrent(true)); $("#run-validation-empty")?.addEventListener("click",()=>validateCurrent(true)); $("#rerun-validation")?.addEventListener("click",()=>validateCurrent(true)); $("#validate-run")?.addEventListener("click",runCurrent);
}

const metricLabels = { "completed_turnarounds":"Turnarounds completed", "total_turnarounds":"Total turnarounds", "avg_departure_delay_minutes":"Average departure delay", "avg_turnaround_minutes":"Average turnaround", "surface_taxi_distance_m":"Taxi distance", "surface_taxi_seconds":"Taxi time", "surface_wait_seconds":"Surface waiting", "runway_queue_seconds":"Runway queue time", "surface_reroutes":"Reroutes", "surface_arrived_aircraft":"Arrivals completed", "surface_departed_aircraft":"Departures completed", "fleet_collisions":"Fleet collisions", "surface_aircraft_aircraft_collisions":"Aircraft collisions", "surface_aircraft_ground_collisions":"Aircraft / ground collisions", "minimum_aircraft_separation_m":"Minimum aircraft separation" };
function metricsFor(run){const raw=run?.result?.metrics||{};const flat={...raw};for(const section of ["surface","fleet"]){if(raw[section]&&typeof raw[section]==="object")for(const [key,value] of Object.entries(raw[section]))flat[`${section}_${key}`]=value;}return Object.entries(metricLabels).filter(([key])=>flat[key]!==undefined).map(([key,label])=>({key,label,value:flat[key]}));}
function renderResults() {
  const completed=state.runs.filter(run=>run.status==="complete"),inProgress=state.runs.filter(run=>["queued","preparing","generating","running"].includes(run.status));
  const choice=(run,selected)=>`<option value="${esc(run.id)}" ${run.id===selected?"selected":""}>${esc(run.scenario_name)} · seed ${esc(run.seed)} · ${esc(run.created_at)}</option>`;
  const baselineChoices=completed.map(run=>choice(run,state.compareIds?.baseline_id)).join("");
  const variantChoices=completed.map(run=>choice(run,state.compareIds?.variant_id)).join("");
  const comparison=state.compare;
  $("#view-results").innerHTML=`<div class="panel" style="margin-bottom:14px"><div class="panel-title"><div><h2>Scenario runs</h2><p>Every row is backed by native RampLab output and Goal 19 analysis artifacts.</p></div><span class="badge">${state.runs.length} saved runs</span></div>${inProgress.map(run=>`<div class="run-row"><div class="row-main"><strong>${esc(run.scenario_name)}</strong><small>${esc(run.progress||run.status)}${run.message?` · ${esc(run.message)}`:""}</small>${["queued","preparing","generating","running"].includes(run.status)?`<div class="run-progress"><span></span></div>`:""}</div><span class="result-status ${run.status=== "failed"?"failed":run.status!=="complete"?"running":""}">${esc(run.status)}</span></div>`).join("")}${completed.map(run=>{const values=metricsFor(run).slice(0,4);return `<div class="run-row"><div class="row-main"><strong>${esc(run.scenario_name)}</strong><small>${esc(run.airport_key.toUpperCase())} · seed ${esc(run.seed)} · ${esc(run.created_at)} · ${esc(run.result?.wall_seconds??"—")} sec wall time</small><small>${values.map(item=>`${esc(item.label)}: ${esc(formatMetric(item.key,item.value))}`).join(" · ")}</small></div><div class="row-actions"><span class="result-status">Complete</span><button class="text-button viewer-run" data-run="${esc(run.id)}">Open in Unreal</button><button class="text-button export-run" data-run="${esc(run.id)}">Generated files</button></div></div>`;}).join("")}${state.runs.some(run=>run.status==="failed")?state.runs.filter(run=>run.status==="failed").map(run=>`<div class="notice warning" style="margin-top:9px"><strong>${esc(run.scenario_name)} failed</strong><br>${esc(run.message||"No simulator error details were returned.")}</div>`).join(""):""}${!state.runs.length?`<div class="empty-state"><span class="empty-symbol">▥</span><h3>No runs yet</h3><p>Validate the airport scenario and run it to create real simulator results.</p></div>`:""}</div><div class="panel"><div class="panel-title"><div><h2>Baseline comparison</h2><p>Only metrics present in both simulator exports are compared. Missing safety evidence remains unknown.</p></div></div><div class="compare-controls"><div class="field"><label for="compare-baseline">Baseline run</label><select id="compare-baseline" ${completed.length<2?"disabled":""}>${baselineChoices}</select></div><div class="field"><label for="compare-variant">Variant run</label><select id="compare-variant" ${completed.length<2?"disabled":""}>${variantChoices}</select></div><button id="compare-button" class="button outline" ${completed.length<2?"disabled":""}>Compare runs</button></div>${completed.length<2?`<div class="notice">Complete a baseline run and a variant run to compare operational metrics.</div>`:""}${comparison?renderComparison(comparison):""}</div>`;
  $("#compare-button")?.addEventListener("click",compareSelected);
  $$(".export-run").forEach(button=>button.addEventListener("click",()=>downloadRun(button.dataset.run)));
  $$(".viewer-run").forEach(button=>button.addEventListener("click",async()=>{try{const result=await api("/api/studio/viewer",{run_id:button.dataset.run});toast(`${result.message} (process ${result.pid}).`);}catch(error){toast(error.message,true);}}));
  if(inProgress.length&&!state.pollTimer) state.pollTimer=setInterval(pollRuns,1600);
}

function formatMetric(key,value){if(typeof value!=="number")return String(value);if(key.includes("delay_minutes")||key.includes("turnaround_minutes"))return `${value.toFixed(1)} min`;if(key.endsWith("_m"))return `${value.toFixed(0)} m`;if(key.endsWith("_seconds"))return `${value.toFixed(0)} s`;return Number.isInteger(value)?String(value):value.toFixed(2);}
function renderComparison(result){
  const metrics=result.metrics||[];const safety=result.safety||{status:"unknown",findings:[]};
  return `<div class="notice ${safety.status==="regression"?"warning":""}" style="margin:12px 0"><strong>Safety assessment: ${esc(safety.status.replaceAll("_"," ").toUpperCase())}</strong><br>${safety.findings?.map(item=>esc(item.finding)).join(" · ")||"No comparable safety findings are available."}</div>${metrics.length?`<div class="table-wrap"><table class="studio-table metric-table"><thead><tr><th>Metric</th><th>Baseline</th><th>Variant</th><th>Change</th><th>Impact</th></tr></thead><tbody>${metrics.map(item=>`<tr><td>${esc(item.label)}</td><td>${esc(formatMetric(item.key,item.baseline))}</td><td>${esc(formatMetric(item.key,item.comparison))}</td><td>${item.difference>0?"+":""}${esc(formatMetric(item.key,item.difference))}</td><td class="impact-${esc(item.impact)}">${esc(item.impact)}</td></tr>`).join("")}</tbody></table></div>`:`<div class="notice">INSUFFICIENT DATA · The runs have no shared metric fields for comparison.</div>`}`;
}

async function pollRuns() {
  try {const result=await api("/api/studio/runs");state.runs=result.runs||[];if(!state.runs.some(run=>["queued","preparing","generating","running"].includes(run.status))){clearInterval(state.pollTimer);state.pollTimer=null;toast("Scenario run finished.");}if(state.view==="results")renderResults();}
  catch(error){clearInterval(state.pollTimer);state.pollTimer=null;toast(error.message,true);}
}

async function runCurrent() {
  const result=state.validation?.valid?state.validation:await validateCurrent(false);
  if(!result)return;
  if(!result.valid){toast("Resolve validation errors before running.",true);nav("validation");return;}
  try{const response=await api("/api/studio/run",{scenario:state.scenario});state.runs.unshift(response.run);nav("results");toast("RampLab simulation started.");if(!state.pollTimer)state.pollTimer=setInterval(pollRuns,1600);}
  catch(error){toast(error.message,true);}
}

async function compareSelected() {
  const baseline=$("#compare-baseline").value,variant=$("#compare-variant").value;
  if(!baseline||!variant||baseline===variant){toast("Choose two different completed runs.",true);return;}
  try{const response=await api("/api/studio/compare",{baseline_id:baseline,variant_id:variant});state.compare=response.comparison;state.compareIds={baseline_id:baseline,variant_id:variant};renderResults();}
  catch(error){toast(error.message,true);}
}

async function saveScenario() {
  state.scenario.name=$("#scenario-name").value.trim()||"Untitled scenario";
  try{const result=await api("/api/studio/save",{scenario:state.scenario});state.scenario=result.scenario;toast("Scenario saved on this computer.");}
  catch(error){toast(error.message,true);}
}

async function openSaved() {
  try{const result=await api("/api/studio/scenarios");const list=result.scenarios||[];$("#saved-list").innerHTML=list.length?list.map(item=>`<div class="saved-row"><div class="row-main"><strong>${esc(item.name)}</strong><small>${esc((item.airport_key||"kauo").toUpperCase())} · ${esc(item.disruptions)} active changes · saved ${esc(item.saved_at||"")}</small></div><button class="button outline open-saved-item" data-id="${esc(item.id)}">Open</button></div>`).join(""):`<div class="empty-state"><span class="empty-symbol">□</span><h3>No saved scenarios yet</h3><p>Save a variant to keep it available for a later session.</p></div>`;
    $$(".open-saved-item").forEach(button=>button.addEventListener("click",async()=>{try{const saved=await api(`/api/studio/scenarios/${button.dataset.id}`);state.scenario=saved.scenario;state.boot.scenario=state.scenario;state.validation=null;state.selectedEdge=null;$("#saved-dialog").close();render();toast("Saved scenario opened.");}catch(error){toast(error.message,true);}}));$("#saved-dialog").showModal();}
  catch(error){toast(error.message,true);}
}

function openConfirm(title,copy,onAccept){const dialog=$("#confirm-dialog");$("#confirm-title").textContent=title;$("#confirm-copy").textContent=copy;const button=$("#confirm-accept");const listener=()=>{button.removeEventListener("click",listener);dialog.close("accept");onAccept();};button.addEventListener("click",listener);dialog.showModal();}

function setAirport(key){if(key===state.scenario.airport_key)return;openConfirm("Change airport dataset?","The current unsaved edits will be replaced by a new baseline for the selected dataset.",()=>loadAirport(key));}

function createVariant(){const original=state.scenario;state.scenario=clone(original);state.scenario.id="";state.scenario.name=`${original.name.replace(/\s+(variant|copy)\s*\d*$/i,"")} variant`;state.scenario.baseline=false;state.scenario.created_at=new Date().toISOString();state.validation=null;$("#scenario-name").value=state.scenario.name;toast("Variant created. Your baseline is unchanged.");render();}

async function exportScenario(){
  try{const response=await fetch("/api/studio/export",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({scenario:state.scenario})});if(!response.ok){const error=await response.json();throw new Error(error.error||"Export failed.");}const blob=await response.blob();const href=URL.createObjectURL(blob);const link=document.createElement("a");link.href=href;link.download=`${state.scenario.airport_key}-scenario-files.zip`;link.click();URL.revokeObjectURL(href);toast("RampLab scenario and provenance exported.");}
  catch(error){toast(error.message,true);}
}

async function downloadRun(id){
  try{const response=await fetch(`/api/studio/runs/${id}/export`);if(!response.ok)throw new Error("Run files are not available.");const blob=await response.blob(),href=URL.createObjectURL(blob),link=document.createElement("a");link.href=href;link.download="ramplab-run-artifacts.zip";link.click();URL.revokeObjectURL(href);}
  catch(error){toast(error.message,true);}
}

function wireEvents(){
  $$(".nav-item").forEach(button=>button.addEventListener("click",()=>nav(button.dataset.view)));
  $("#airport-select").addEventListener("change",event=>setAirport(event.target.value));
  $("#scenario-name").addEventListener("input",event=>{if(state.scenario)state.scenario.name=event.target.value;});
  $("#new-variant").addEventListener("click",createVariant); $("#save-scenario").addEventListener("click",saveScenario); $("#open-saved").addEventListener("click",openSaved); $("#export-scenario").addEventListener("click",exportScenario);
  $("#fit-map").addEventListener("click",()=>{state.selectedEdge=null;render();}); $("#map-disruptions").addEventListener("click",()=>nav("disruptions")); $("#validate-button").addEventListener("click",()=>validateCurrent(true)); $("#run-button").addEventListener("click",runCurrent);
  $("#main-nav").addEventListener("keydown",event=>{if(event.key==="ArrowDown"||event.key==="ArrowUp"){const items=$$(".nav-item"),index=items.indexOf(document.activeElement),next=(index+(event.key==="ArrowDown"?1:-1)+items.length)%items.length;items[next].focus();}});
}

wireEvents();
loadAirport("kauo");
