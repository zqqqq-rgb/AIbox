"""Verify real importance writes through the UI, then restore the original flag.

Requires a running development demo with authentication disabled.
"""
import argparse
import json
from pathlib import Path

from playwright.sync_api import expect, sync_playwright


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--base', default='http://localhost:8080')
    parser.add_argument('--event-id', type=int, default=12)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    output = Path(args.output)
    output.mkdir(parents=True, exist_ok=True)
    checks, errors = [], []
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(headless=True, executable_path=playwright.chromium.executable_path)
        context = browser.new_context(viewport={'width': 1440, 'height': 1000})
        api_path = f'/api/events/{args.event_id}'

        def saved():
            response = context.request.get(args.base + api_path).json()
            assert response['code'] == 0, response
            return response['data']

        original = saved()
        assert isinstance(original.get('important'), bool), 'Run the new backend first'
        original_flag = original['important']
        pages = [context.new_page(), context.new_page()]

        def dialog(page):
            return page.get_by_role('dialog', name=f'事件 #{args.event_id} 详情', exact=True)

        def open_detail(page):
            page.locator(f'.ev-row[data-event-id="{args.event_id}"]').get_by_role('button', name='查看详情', exact=True).click()
            expect(dialog(page).locator('.event-detail-fields')).to_be_visible()

        def toggle(page, current):
            dialog(page).get_by_role('button', name='取消重要' if current else '标记重要', exact=True).click()

        def expect_flag(page, value):
            expect(dialog(page).get_by_role('button', name='取消重要' if value else '标记重要', exact=True)).to_be_enabled()
            badge = page.locator(f'.ev-row[data-event-id="{args.event_id}"] .event-important-badge')
            expect(badge).to_be_visible() if value else expect(badge).to_be_hidden()

        try:
            for page in pages:
                page.set_default_timeout(8000)
                page.on('pageerror', lambda error: errors.append(str(error)))
                page.goto(args.base + '/#/events')
                open_detail(page)
                expect_flag(page, original_flag)
            primary, stale = pages
            toggle(primary, original_flag)
            expect_flag(primary, not original_flag)
            expect(dialog(primary).get_by_role('status')).to_contain_text('已')
            marked = saved()
            assert marked['important'] != original_flag
            assert marked['important_revision'] == original['important_revision'] + 1
            for key in ('camera_id', 'task_id', 'rule_id', 'task_revision', 'rule_version', 'targets', 'reported'):
                assert marked[key] == original[key], key
            primary.screenshot(path=str(output / 'marked-important.png'))
            primary.reload()
            open_detail(primary)
            expect_flag(primary, not original_flag)
            checks.append('real UI write updates list badge and survives page reload; detection data unchanged')

            toggle(stale, original_flag)
            expect(dialog(stale).get_by_role('status')).to_contain_text('其他页面已修改')
            assert saved()['important_revision'] == marked['important_revision']
            stale.screenshot(path=str(output / 'revision-conflict.png'))
            dialog(stale).get_by_role('button', name='重新加载详情', exact=True).click()
            expect_flag(stale, not original_flag)
            toggle(stale, not original_flag)
            expect_flag(stale, original_flag)
            checks.append('stale second page is rejected; reload then cancellation persists')

            def fail_save(route):
                route.fulfill(status=200, json={'code': 5001, 'msg': 'simulated storage failure'})

            primary.route('**' + api_path + '/important', fail_save)
            toggle(primary, not original_flag)
            expect(dialog(primary).get_by_role('status')).to_contain_text('保存未确认')
            expect(dialog(primary).get_by_role('button', name='保存未完成', exact=True)).to_be_disabled()
            primary.unroute('**' + api_path + '/important', fail_save)
            dialog(primary).get_by_role('button', name='重新加载详情', exact=True).click()
            expect_flag(primary, original_flag)
            primary.reload()
            open_detail(primary)
            expect_flag(primary, original_flag)
            checks.append('save failure shows no success and requires reloading authoritative state')
            assert not errors, errors
            final = saved()
            assert final['important'] == original_flag
            (output / 'browser.json').write_text(json.dumps({
                'checks': checks, 'page_errors': errors, 'event_id': args.event_id,
                'original_important': original_flag, 'final_important': final['important'],
                'original_revision': original['important_revision'], 'final_revision': final['important_revision'],
            }, ensure_ascii=False, indent=2), encoding='utf-8')
        finally:
            # Restore the user's original flag even if an assertion fails. History remains accurate.
            current = saved()
            if current['important'] != original_flag:
                restored = context.request.put(args.base + api_path + '/important', data={
                    'important': original_flag, 'revision': current['important_revision'],
                }).json()
                assert restored['code'] == 0, restored
            browser.close()
    print(json.dumps({'passed': len(checks), 'page_errors': errors, 'original_flag_restored': True}))


if __name__ == '__main__':
    main()
