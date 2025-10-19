# Настройка окружения

Для решения задач вам потребуется компьютер с Unix окружением. Лучше всего использовать Linux.
OSX тоже будет нормально работать.

Можно использовать Windows, но для компиляции и запуска потребуется WSL (Windows Subsystem for Linux).
В этом случае сначала установите WSL, а дальшейшие инструкции выполняйте уже в Linux окружении.

Рекомендуется использовать Ubuntu версии **22.04** (или **24.04**).
Тестирующая система проверяет задачи на этой
версии Ubuntu, и если вы будете использовать эту же версию локально,
то вы избежите проблем со старыми версиями компиляторов.
Минимальные версии поддерживаемых компиляторов и способ их установки в Ubuntu:

- **g++-14**
```bash
$ sudo apt-get install g++-14
```
- **clang++-20**
```bash
$ wget https://apt.llvm.org/llvm.sh
$ chmod +x llvm.sh
$ sudo ./llvm.sh 20 all
```

## Регистрация в системе

1. Зарегистрируйтесь в системе [manytask](https://mipt-caos-fall.hsse.manytask.org). Код регистрации доступен в чате курса.
Если вы ранее регистрировались в этой системе для прохождения других курсов, то сразу можете жать кнопку **Login**.
2. Сгенерируйте ssh ключ, если у вас его еще нет.
   ```bash
   $ ssh-keygen -N "" -f ~/.ssh/id_rsa
   ```
3. Скопируйте содержимое файла `id_rsa.pub` (`cat ~/.ssh/id_rsa.pub`) в [список SSH ключей в gitlab](https://gitlab.manytask.org/-/user_settings/ssh_keys).
4. Проверьте, что ssh ключ работает. Выполните команду `ssh git@gitlab.manytask.org`. Вы должны увидеть такое приветствие:
   ```bash
   $ ssh git@gitlab.manytask.org
   PTY allocation request failed on channel 0
   Welcome to GitLab, USERNAME!
   Connection to gitlab.manytask.org closed.
   ```
   Если вы раньше не взаимодействовали с `gitlab.manytask.org` через SSH, у вас может появиться сообщение с вопросом, хотите ли вы подключиться к неизвестному хосту. Выглядит оно примерно так:
   ```
   The authenticity of host 'gitlab.manytask.org' can't be established.
   ...
   Are you sure you want to continue connecting (yes/no)?
   ```
   Нужно согласиться с подключением, то есть написать `yes`.

## Настройка локального репозитория

1. На странице [manytask](https://mipt-caos-fall.hsse.manytask.org) нажмите на **My repo**
2. Нажмите на синию кнопку **Code** и скопируйте SSH адрес
3. Используя консоль склонируйте и настройте репозиторий с задачами:
```bash
$ git clone "<Полученный SSH адрес>"

# Переходим в директорию склонированного репозитория
$ cd "<Склонированный репозиторий>"

# Настраиваем пользователя
$ git config --local user.name "<логин с manytask>"
$ git config --local user.email "<email с manytask>"
```

В течении всего курса публичный репозиторий будет обновляться, в нём будут появляться новые задачи,
а также возможно будут обновляться тесты и условия существующих задач. 
Данные изменения будут **автоматически** добавляться в ваш репозиторий. Чтобы подтянуть изменения в локальную копию:
```bash
$ git checkout main
$ git pull
```
После этого, если у вас есть уже написанные/отосланные задачи, по которым ревью ещё не пройдено, зайдите в ветку каждой и обновите её:
```bash
$ git checkout "<название ветки>"
$ git rebase main
```

## Посылка тестовой задачи в систему

1. Настройте IDE. Если вы используйте Windows, то вам также надо настроить WSL:
   * CLion
      - \[Windows\] [Настройка WSL](https://www.jetbrains.com/help/clion/how-to-use-wsl-development-environment-in-clion.html)
      - Настройку можно посмотреть в [записи семинара ШАДа](https://disk.yandex.ru/i/8waWBV-L-FOKOw)
   * VS Code
      - \[Windows\] [Настройка WSL](https://code.visualstudio.com/docs/cpp/config-wsl)
      - Настройка описана в [инструкции](https://docs.google.com/document/d/1mb4oGIJcU1vkNHoW0lHV_MsiolrWoLdG/edit)

2. Решите первую задачу [multiplication](../multiplication).

3. Сдайте тестовое задание `multiplication` в систему:
```bash
# Находясь в корне репозитория
$ TASK=multiplication
$ git checkout -b submit/$TASK
$ git add $TASK/*
$ git commit -m "Add $TASK task"
$ git push origin submit/$TASK
```
Для сдачи других задач замените значение переменной `TASK` выше на название задачи (оно совпадает с названием папки с задачей, а также с именем задачи в [manytask](https://mipt-caos-fall.hsse.manytask.org)).

4. Пронаблюдайте за процессом тестирования на странице CI/CD -> Pipelines своего репозитория. gitlab показывает вывод консоли во время тестирования.

5. Проверьте, что оценка появилась в [таблице с результатами](https://docs.google.com/spreadsheets/d/1d_5aNFEy0EEh7gBwan2bSU_k8HgD4oUsityRH074VQg), а сама задача покрасилась на странице [manytask](https://mipt-caos-fall.hsse.manytask.org).

6. Создайте Merge Request из ветки с задачей в ветку `main`. Не добавляйте никого в Assignee и Reviewers, система сделает это автоматически. 

**Внимание**: запрещено самостоятельно менять поля Assignee и Reviewers после их назначения системой, а также закрывать Merge Request. При выявлении нарушения будут санкции.

7. Проверьте, что статус задачи в [таблице](https://docs.google.com/spreadsheets/d/1d_5aNFEy0EEh7gBwan2bSU_k8HgD4oUsityRH074VQg) изменился.

## Статус задачи

| Статус | Цвет в [manytask](https://mipt-caos-fall.hsse.manytask.org) | Символ в [таблице](https://docs.google.com/spreadsheets/d/1d_5aNFEy0EEh7gBwan2bSU_k8HgD4oUsityRH074VQg) |
| ------ | ------ | ------|
| Не отправлена | Серый | ` ` |
| Тесты пройдены | Жёлтый | `#` |
| Тесты пройдены и есть MR | Жёлтый | `?` |
| Ревью не пройдено | Красный | `-` |
| Ревью пройдено* | Зелёный | `+` |

\* - только в этом случае задача считается целиком сданной.

## Установка и запуск линтера

```bash
$ wget https://apt.llvm.org/llvm.sh
$ chmod +x llvm.sh
$ sudo ./llvm.sh 20 all
```

```bash
# Для доступа к утилите линтера без версии (необходимо на Ubuntu)
$ sudo ln -s /usr/bin/clang-format-20 /usr/bin/clang-format 
$ sudo ln -s /usr/bin/clang-tidy-20 /usr/bin/clang-tidy
```

```bash
# Находясь в build директории
../run_linter.sh <имя задачи>
```
