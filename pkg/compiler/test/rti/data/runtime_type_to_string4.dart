// Copyright (c) 2018, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

// @dart = 2.7

/*spec:nnbd-off|spec:nnbd-sdk.class: Class1:needsArgs*/
class Class1<T> {
  /*spec:nnbd-off|prod:nnbd-off.member: Class1.:*/
  Class1();
}

/*spec:nnbd-off|prod:nnbd-off.class: Class2:*/
class Class2<T> {
  /*spec:nnbd-off|prod:nnbd-off.member: Class2.:*/
  Class2();
}

/*spec:nnbd-off|prod:nnbd-off.member: main:*/
main() {
  Class1<int> cls1 = new Class1<int>();
  print(cls1?.runtimeType?.toString());
  new Class2<int>();
}
