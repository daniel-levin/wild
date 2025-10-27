use criterion::Criterion;
use criterion::criterion_group;
use criterion::criterion_main;
use libwild::layout_rules::LayoutRulesBuilder;
use linker_utils::elf::SectionFlags;
use linker_utils::elf::sht;

fn layout_rules(c: &mut Criterion) {
    let lr = LayoutRulesBuilder::default().build();
    c.bench_function("layout_rules_lookup_hit", |b| {
        b.iter(|| {
            lr.section_rules
                .lookup(b".text", SectionFlags::empty(), sht::PROGBITS);
        })
    });

    c.bench_function("layout_rules_lookup_miss", |b| {
        b.iter(|| {
            lr.section_rules
                .lookup(b".nonexistent", SectionFlags::empty(), sht::PROGBITS);
        })
    });

    c.bench_function("layout_rules_lookup_mixed", |b| {
        b.iter(|| {
            for r in ["text", ".text.some-long-name", "", "abc"] {
                lr.section_rules
                    .lookup(r.as_bytes(), SectionFlags::empty(), sht::PROGBITS);
            }
        })
    });
}

criterion_group!(benches, layout_rules);
criterion_main!(benches);
