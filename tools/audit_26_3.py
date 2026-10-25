"""Rebuild static reference inventories. These are evidence indexes, not parity tests."""
from pathlib import Path
import argparse
import csv
import hashlib
import json
import re
import subprocess
from datetime import datetime, timezone


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--reference', type=Path, default=Path(r'C:\Users\Admin\Desktop\Minecraft Split Screen\decompiled-26.3'))
    ap.add_argument('--output', type=Path, default=Path('docs/audit-26.3'))
    args = ap.parse_args()
    root = Path(__file__).resolve().parents[1]
    ref = args.reference.resolve()
    out = (root / args.output).resolve()
    out.mkdir(parents=True, exist_ok=True)
    cpp_bytes = {p:p.read_bytes() for p in sorted((root/'src').rglob('*')) if p.suffix in ('.cpp', '.h', '.inc')}
    cpp = {p:b.decode('utf-8', errors='replace') for p,b in cpp_bytes.items()}
    java = sorted(ref.rglob('*.java'))
    def refs(pattern, sources=cpp):
        hits = []
        for p, text in sources.items():
            for m in re.finditer(pattern, text):
                hits.append(f'{p.relative_to(root).as_posix()}:{text.count(chr(10), 0, m.start())+1}')
        return '; '.join(hits)
    def emit(name, rows, columns):
        with (out/name).open('w', newline='', encoding='utf-8') as f:
            w = csv.DictWriter(f, fieldnames=columns)
            w.writeheader()
            w.writerows(rows)
    opt = ref/'net/minecraft/client/Options.java'
    text = opt.read_text(encoding='utf-8')
    options = []
    own_keys = set(re.findall(r'k == "([^"]+)"', cpp[root/'src/ui/ui.cpp']))
    aliases = {'enableVsync':'vsync'}
    for m in re.finditer(r'access\.process\("([^"]+)"\s*,\s*this\.([\w]+)', text):
        key, field = m.groups()
        own = aliases.get(key,key)
        options.append({'key':key,'java_field':field,'reference':f'net/minecraft/client/Options.java:{text.count(chr(10),0,m.start())+1}', 'cpp_persistence':'present' if own in own_keys else 'absent', 'cpp_evidence':refs(r'k == "'+re.escape(own)+r'"')})
    emit('settings.csv', options, ['key','java_field','reference','cpp_persistence','cpp_evidence'])
    keys = []
    for m in re.finditer(r'public final KeyMapping (\w+) = new (?:Toggle)?KeyMapping\(\s*"([^"]+)"',text):
        keys.append({'field':m[1],'translation_key':m[2],'reference':f'net/minecraft/client/Options.java:{text.count(chr(10),0,m.start())+1}', 'status':'no configurable binding table; inspect hardcoded actions'})
    emit('keybindings.csv', keys, ['field','translation_key','reference','status'])
    packets=[]
    for p in sorted((ref/'net/minecraft/network/protocol').rglob('*PacketTypes.java')):
        text=p.read_text(encoding='utf-8')
        for m in re.finditer(r'PacketType<([^;]+?)>\s+\w+\s*=\s*create(Clientbound|Serverbound)\(\s*"([^"]+)"',text):
            cls,flow,name=m.groups()
            handlers=refs(r'(?:onPlay(?:Peek)?|on)\(\s*"'+re.escape(name)+r'"|name == "'+re.escape(name)+r'"') if flow=='Clientbound' else refs(r'(?:sendPlay|send|switchPhase)\([^;\n]*"'+re.escape(name)+r'"')
            packets.append({'phase':p.parent.name,'flow':flow,'name':name,'class':cls.strip(),'reference':f'{p.relative_to(ref).as_posix()}:{text.count(chr(10),0,m.start())+1}','direct_cpp_handler_or_send':handlers,'status':'direct evidence; manual review required' if handlers else 'no direct handler/send found; inspect indirect routing'})
    emit('packets.csv',packets,['phase','flow','name','class','reference','direct_cpp_handler_or_send','status'])
    screens=[{'class':p.stem,'reference':p.relative_to(ref).as_posix(),'status':'reference inventory; manually map to C++ screen/module'} for p in java if p.name.endswith('Screen.java') and ('client' in p.parts)]
    implemented = {
        'StatsScreen':'src/ui/stats.cpp', 'AdvancementsScreen':'src/ui/advancements.cpp',
        'ChatScreen':'src/ui/hud.cpp; src/ui/chat_ext.cpp', 'DeathScreen':'src/ui/screens.cpp',
        'GameModeSwitcherScreen':'src/ui/bed_gamemode.cpp', 'InBedChatScreen':'src/ui/bed_gamemode.cpp',
        'AnvilScreen':'src/ui/stations_a.cpp', 'BeaconScreen':'src/ui/stations_b.cpp',
        'BlastFurnaceScreen':'src/ui/screens.cpp', 'BookEditScreen':'src/ui/signs_books.cpp',
        'BookSignScreen':'src/ui/signs_books.cpp', 'BookViewScreen':'src/ui/signs_books.cpp',
        'BrewingStandScreen':'src/ui/stations_b.cpp', 'CartographyTableScreen':'src/ui/stations_a2.cpp',
        'ContainerScreen':'src/ui/screens.cpp', 'CrafterScreen':'src/ui/stations_a2.cpp',
        'CraftingScreen':'src/ui/screens.cpp (recipe book missing)', 'CreativeModeInventoryScreen':'src/ui/creative.cpp',
        'DispenserScreen':'src/ui/screens.cpp', 'EnchantmentScreen':'src/ui/stations_a.cpp',
        'FurnaceScreen':'src/ui/screens.cpp (recipe book missing)', 'GrindstoneScreen':'src/ui/stations_a.cpp',
        'HangingSignEditScreen':'src/ui/signs_books.cpp', 'HopperScreen':'src/ui/screens.cpp',
        'HorseInventoryScreen':'src/ui/stations_b.cpp', 'InventoryScreen':'src/ui/screens.cpp (recipe book missing)',
        'LecternScreen':'src/ui/stations_b.cpp', 'LoomScreen':'src/ui/stations_a2.cpp',
        'MerchantScreen':'src/ui/stations_b.cpp', 'NautilusInventoryScreen':'src/ui/stations_b.cpp',
        'ShulkerBoxScreen':'src/ui/screens.cpp', 'SignEditScreen':'src/ui/signs_books.cpp',
        'SmithingScreen':'src/ui/stations_b.cpp', 'SmokerScreen':'src/ui/screens.cpp (recipe book missing)',
        'StonecutterScreen':'src/ui/stations_a2.cpp', 'OptionsScreen':'src/ui/screens.cpp (small settings subset)',
        'VideoSettingsScreen':'src/ui/video_screen.cpp (custom renderer options)',
        'PauseScreen':'src/ui/screens.cpp (does not pause local server)',
        'TitleScreen':'src/ui/title.cpp (services/credits missing)',
        'CreateWorldScreen':'src/ui/worlds.cpp (one-page subset)',
        'EditWorldScreen':'src/ui/worlds.cpp (rename/open-folder subset)',
        'SelectWorldScreen':'src/ui/worlds.cpp', 'JoinMultiplayerScreen':'src/ui/servers.cpp',
        'DirectJoinServerScreen':'src/ui/servers.cpp', 'PackSelectionScreen':'src/ui/mods_screen.cpp (custom equivalent)',
    }
    generic = {'PopupScreen','AlertScreen','BackupConfirmScreen','ConfirmScreen','ConnectScreen',
               'DisconnectedScreen','ErrorScreen','GenericMessageScreen','GenericWaitingScreen',
               'LevelLoadingScreen','ProgressScreen','NoticeWithLinkScreen'}
    bases = {'Screen','OptionsSubScreen','ItemCombinerScreen','DialogScreen','AbstractGameRulesScreen',
             'AbstractMountInventoryScreen','AbstractSignEditScreen','AbstractContainerScreen','AbstractFurnaceScreen'}
    for row in screens:
        cls=row['class']
        row['status'] = 'partial/native equivalent: '+implemented[cls] if cls in implemented else 'generic confirmation/loading/message equivalent; specialized flow incomplete' if cls in generic else 'shared Java infrastructure; no independent player-facing screen' if cls in bases else 'missing native screen/flow; see AUDIT.md for priority and scope'
    emit('screens.csv',screens,['class','reference','status'])
    realms=[{'class':p.stem,'reference':p.relative_to(ref).as_posix(),'status':'Realms service/UI absent; account-service scope'} for p in java if p.name.endswith('Screen.java') and 'realmsclient' in p.parts]
    emit('realms-screens.csv',realms,['class','reference','status'])
    registries = [
        ('particles','net/minecraft/core/particles/ParticleTypes.java',r'register\(\s*"([^"]+)"','src/fx/particles.cpp'),
        ('components','net/minecraft/core/component/DataComponents.java',r'register\(\s*"([^"]+)"','src/ui/items.cpp'),
        ('features','net/minecraft/world/level/levelgen/feature/FeatureTypes.java',r'Registry\.register\(registry,\s*"([^"]+)"',None),
        ('processors','net/minecraft/world/level/levelgen/structure/templatesystem/StructureProcessorTypes.java',r'Registry\.register\(registry,\s*"([^"]+)"',None),
        ('structures','net/minecraft/world/level/levelgen/structure/StructureType.java',r'register\(\s*"([^"]+)"',None),
        ('menus','net/minecraft/world/inventory/MenuType.java',r'register\(\s*"([^"]+)"',None),
        ('item-special-renderers','net/minecraft/client/renderer/special/SpecialModelRenderers.java',r'(?:put|register)\(\s*(?:Identifier\.withDefaultNamespace\()?"([^"]+)"',None),
        ('entity-renderers','net/minecraft/client/renderer/entity/EntityRenderers.java',r'register\(EntityTypes\.([A-Z_]+)', 'src/entity/entity_render.cpp'),
        ('block-entity-renderers','net/minecraft/client/renderer/blockentity/BlockEntityRenderers.java',r'register\(BlockEntityTypes\.([A-Z_]+)', 'src/blockentity/block_entities.cpp'),
        ('trunk-placers','net/minecraft/world/level/levelgen/feature/trunkplacers/TrunkPlacerType.java',r'register\(\s*"([^"]+)"',None),
        ('foliage-placers','net/minecraft/world/level/levelgen/feature/foliageplacers/FoliagePlacerType.java',r'register\(\s*"([^"]+)"',None),
        ('item-conditions','net/minecraft/client/renderer/item/properties/conditional/ConditionalItemModelProperties.java',r'withDefaultNamespace\("([^"]+)"',None),
        ('item-ranges','net/minecraft/client/renderer/item/properties/numeric/RangeSelectItemModelProperties.java',r'withDefaultNamespace\("([^"]+)"',None),
        ('item-selects','net/minecraft/client/renderer/item/properties/select/SelectItemModelProperties.java',r'withDefaultNamespace\("([^"]+)"',None),
        ('item-tints','net/minecraft/client/color/item/ItemTintSources.java',r'withDefaultNamespace\("([^"]+)"',None),
    ]
    registry_counts={}
    for category,rel,pat,target in registries:
        p=ref/rel
        if not p.exists():
            registry_counts[category]='source missing'
            continue
        text=p.read_text(encoding='utf-8')
        rows=[]
        sources={root/target:cpp[root/target]} if target else cpp
        for m in re.finditer(pat,text):
            name=m[1].lower()
            hits=refs(r'"(?:minecraft:)?'+re.escape(name)+r'"',sources)
            status='name found; not proof of behavior' if hits else 'name absent; inspect generic/data-driven implementation'
            if category=='components':
                body=cpp[root/target].split('bool Items::skipValue',1)[1].split('bool Items::readPatch',1)[0]
                explicit=bool(re.search(r'"'+re.escape(name)+r'"',body))
                status='explicit skip/decode branch; semantics need review' if explicit else 'generic slash-name varint branch; codec needs review' if '/' in name else 'unsupported: skipValue falls through to false'
            if category=='particles':
                body=cpp[root/target].split('kKinds[]',1)[1].split('kOptions[]',1)[0]
                explicit=bool(re.search(r'"'+re.escape(name)+r'"',body))
                status='explicit particle kind; behavior needs review' if explicit else 'generic particle kind; specialized behavior absent'
                if name in ('item','vibration'): status='network arguments explicitly unsupported'
            if category=='entity-renderers' and (name.endswith('boat') or name.endswith('raft') or name.endswith('minecart')):
                status='generic boat/raft/minecart branch; behavior needs review'
            if category=='entity-renderers' and name in {
                'allay','bat','blaze','breeze','creaking','cushion','endermite','nautilus',
                'shulker','silverfish','sulfur_cube','tadpole','zombie_nautilus'
            }:
                status='default name-based model/texture and Head animation; species behavior incomplete; VIS-01'
            if category=='entity-renderers' and name in {'cushion','sulfur_cube','zombie_nautilus'}:
                status='default texture path absent in vanilla 26.3 JAR; box fallback; VIS-01'
            if category=='entity-renderers' and name in {'item_frame','glow_item_frame','painting'}:
                status='explicit Kind::None; decoration rendering absent; VIS-02'
            if category=='entity-renderers' and name in {'block_display','item_display','text_display'}:
                status='explicit Kind::None; display rendering absent; VIS-03'
            if category=='entity-renderers' and name in {'lightning_bolt','ominous_item_spawner'}:
                status='explicit Kind::None; special geometry absent; VIS-04'
            if category=='entity-renderers' and name in {'marker','interaction'}:
                status='intentionally invisible; not a missing visible renderer'
            if category=='block-entity-renderers' and name in {
                'mob_spawner','piston','end_portal','end_gateway','beacon','structure_block',
                'test_instance_block','brushable_block','trial_spawner','vault'
            }:
                status='reviewed missing native special renderer Kind; VIS-10 through VIS-13'
            if category=='block-entity-renderers' and name in {'sign','hanging_sign','skull','banner','shelf'}:
                status='native family/suffix routing present; item/profile fidelity needs review; VIS-14'
            if category=='menus' and name.startswith('generic_9x'):
                status='generic_9xN prefix routing present; station behavior needs fixtures; ITEM-11'
            if category=='structures' and name in {'end_city','fortress','nether_fossil'}:
                status='reviewed absent native structure factory; delegated in server play; WORLD-01'
            if category=='features':
                registration=refs(r'(?:types\.add\(\s*|addFeature<\w+>\(types,\s*)"minecraft:'+re.escape(name)+r'"')
                status='native factory registration found; exact output/side effects unproved' if registration else 'no native factory registration found'
                hits=registration
            rows.append({'name':name,'reference':f'{rel}:{text.count(chr(10),0,m.start())+1}','cpp_string_evidence':hits,'status':status})
        emit(category+'.csv',rows,['name','reference','cpp_string_evidence','status'])
        registry_counts[category]={'total':len(rows),'without_name_evidence':sum(not r['cpp_string_evidence'] for r in rows)}
        if category=='components': registry_counts[category]['unsupported_decoder_types']=sum(r['status'].startswith('unsupported:') for r in rows)
        if category=='particles': registry_counts[category]['generic_kind_types']=sum(r['status'].startswith('generic particle') for r in rows)
    files=[{'path':p.relative_to(root).as_posix(),'lines':len(t.splitlines()),'sha256':hashlib.sha256(cpp_bytes[p]).hexdigest()} for p,t in cpp.items()]
    emit('source-files.csv',files,['path','lines','sha256'])
    shader_files=[{'path':p.relative_to(root).as_posix(),'lines':len(p.read_text(encoding='utf-8').splitlines()),'sha256':hashlib.sha256(p.read_bytes()).hexdigest()} for p in sorted((root/'shaders').rglob('*')) if p.suffix in ('.hlsl','.hlsli')]
    emit('shader-files.csv',shader_files,['path','lines','sha256'])
    findings=[]
    narrative=out/'AUDIT.md'
    if narrative.exists():
        for m in re.finditer(r'^\| ([A-Z]+-\d+) \| (P[123]) ([^|]+) \| ([^|]+) \| (.*) \|$', narrative.read_text(encoding='utf-8'),re.MULTILINE):
            finding_id,priority,status,title,evidence=m.groups()
            findings.append({'id':finding_id,'priority':priority,'status':status.strip(),
                             'title':title.strip(),'evidence_and_consequence':evidence.strip()})
        emit('backlog.csv',findings,['id','priority','status','title','evidence_and_consequence'])
    summary = {
        'generated_utc': datetime.now(timezone.utc).isoformat(),
        'reference_root': str(ref),
        'version': json.loads((ref/'version.json').read_text()),
        'git_head': subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),
        'working_tree_status': subprocess.check_output(['git','status','--short'],cwd=root,text=True),
        'cpp_source_files': len(cpp),
        'cpp_source_lines': sum(r['lines'] for r in files),
        'shader_files': len(shader_files),
        'reference_java_files': len(java),
        'static_settings': len(options),
        'settings_with_cpp_persistence': sum(r['cpp_persistence']=='present' for r in options),
        'declared_keybindings': len(keys),
        'hotbar_keybindings': 9,
        'reference_client_screens': len(screens),
        'reference_realms_screens': len(realms),
        'reviewed_findings': len(findings),
        'reviewed_priorities': {p:sum(r['priority']==p for r in findings) for p in ('P1','P2','P3')},
        'packets': {
            phase: {
                flow: {
                    'total': sum(p['phase']==phase and p['flow']==flow for p in packets),
                    'without_direct_evidence': sum(p['phase']==phase and p['flow']==flow and not p['direct_cpp_handler_or_send'] for p in packets)
                } for flow in ('Clientbound','Serverbound')
            } for phase in sorted({p['phase'] for p in packets})
        },
        'registries': registry_counts,
        'warning': 'Static inventories measure source evidence only. A matched name is not proof of parity, and absence can be generic/indirect support. Dynamic options families are excluded from static_settings.'
    }
    (out/'inventory.json').write_text(json.dumps(summary,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(summary,indent=2))


if __name__ == '__main__':
    main()
