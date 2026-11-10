"""Read-only event-detail browser acceptance against a running AIBox demo."""
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
    checks, errors, requests = [], [], []
    with sync_playwright() as playwright:
        browser = playwright.chromium.launch(headless=True, executable_path=playwright.chromium.executable_path)
        context = browser.new_context(viewport={'width': 1440, 'height': 1000})
        page = context.new_page()
        page.set_default_timeout(8000)
        page.on('pageerror', lambda error: errors.append(str(error)))
        path = f'/api/events/{args.event_id}'
        response = context.request.get(args.base + path).json()
        assert response['code'] == 0, response
        detail = response['data']
        page.on('request', lambda request: requests.append(request.url) if request.url.endswith(path) else None)
        page.goto(args.base + '/#/events')
        row = page.locator(f'.ev-row[data-event-id="{args.event_id}"]')
        button = row.get_by_role('button', name='查看详情', exact=True)
        expect(button).to_be_enabled()
        page.screenshot(path=str(output / 'frontend-list.png'), full_page=True)
        button.click()
        dialog = page.get_by_role('dialog', name=f'事件 #{args.event_id} 详情', exact=True)
        fields = dialog.locator('.event-detail-fields')

        def field(label):
            return fields.locator('dt', has_text=label).locator('xpath=following-sibling::dd[1]')

        expect(field('事件 ID')).to_have_text(str(detail['id']))
        expect(field('摄像头')).to_contain_text(f"cam{detail['camera_id']}")
        expect(field('任务 ID')).to_have_text(str(detail['task_id']))
        expect(field('规则 ID')).to_have_text(str(detail['rule_id']))
        expect(dialog.get_by_role('heading', name=f"检测目标（{len(detail['targets'])}）")).to_be_visible()
        assert len(requests) == 1, requests
        page.screenshot(path=str(output / 'frontend-detail.png'), full_page=True)
        checks.append('list button fetches persisted event ID and displays live API source and targets')
        dialog.get_by_role('button', name='关闭', exact=True).click()
        expect(dialog).to_have_count(0)
        expect(button).to_be_focused()

        failures = []

        def fail_once(route):
            if not failures:
                failures.append(True)
                route.fulfill(status=200, json={'code': 1002, 'msg': 'event not found'})
            else:
                route.continue_()

        page.route('**' + path, fail_once)
        button.click()
        expect(dialog.get_by_role('alert')).to_contain_text('event not found')
        dialog.get_by_role('button', name='重试', exact=True).click()
        expect(field('事件 ID')).to_have_text(str(detail['id']))
        page.keyboard.press('Escape')
        expect(dialog).to_have_count(0)
        page.unroute('**' + path, fail_once)
        checks.append('query failure stays in dialog; retry recovers; Close and Escape dismiss')

        pending = []
        page.route('**' + path, lambda route: pending.append(route))
        button.click()
        expect(dialog).to_contain_text('正在查询')
        page.wait_for_timeout(100)
        assert pending
        dialog.get_by_role('button', name='关闭', exact=True).click()
        pending[0].fulfill(status=200, json=response)
        page.wait_for_timeout(150)
        expect(dialog).to_have_count(0)
        page.unroute('**' + path)
        checks.append('closing an in-flight query does not reopen the dialog')
        assert not errors, errors
        (output / 'frontend-browser.json').write_text(json.dumps({
            'checks': checks, 'page_errors': errors, 'event_id': detail['id'],
            'camera_id': detail['camera_id'], 'task_id': detail['task_id'],
            'rule_id': detail['rule_id'], 'detail_request_count': len(requests),
        }, ensure_ascii=False, indent=2), encoding='utf-8')
        browser.close()
    print(json.dumps({'passed': len(checks), 'page_errors': errors}))


if __name__ == '__main__':
    main()
