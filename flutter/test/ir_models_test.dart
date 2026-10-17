import 'package:flutter_test/flutter_test.dart';

import 'package:deauther/models/ir_models.dart';

void main() {
  test('universal categories expose only functions in their IR libraries', () {
    final tokens = {
      for (final category in kIrCategories)
        category.id: category.buttons.map((button) => button.token).toSet(),
    };

    expect(tokens['tv'], {
      'power',
      'mute',
      'vol_up',
      'vol_dn',
      'ch_up',
      'ch_dn',
    });
    expect(tokens['ac'], {
      'off',
      'dry',
      'cool_hi',
      'cool_lo',
      'heat_hi',
      'heat_lo',
    });
    expect(tokens['projector'], {'power', 'vol_up', 'vol_dn', 'mute'});
    expect(tokens['led'], {
      'power_on',
      'power_off',
      'bright_up',
      'bright_dn',
      'red',
      'green',
      'blue',
      'white',
    });
    expect(tokens['fan'], {
      'power',
      'speed_up',
      'speed_dn',
      'swing',
      'mode',
      'timer',
    });
    expect(tokens['audio'], {
      'power',
      'vol_up',
      'vol_dn',
      'mute',
      'next',
      'prev',
      'play',
      'pause',
    });
    expect(kIrCategories.every((category) => category.builtIn), isTrue);
  });
}
