import * as PIXI from "pixi.js"
import { Vector2 } from "./krol"
import { PartyMember } from "./entities/PartyMember"

function update(time: PIXI.Ticker) {
  // Check if we have a target position
  player.update(time)
  // if (targetPosition) {
  //   const distanceToTarget = Vector2.fromPoint(player.position)
  //     .sub(targetPosition)
  //     .magnitude()

  //   // If we're close enough to the target, stop moving
  //   if (distanceToTarget < 4) {
  //     playerDirection.x = 0
  //     playerDirection.y = 0
  //     player.position.set(targetPosition.x, targetPosition.y)
  //     targetPosition = null
  //     return
  //   }
  // }

  // playerDirection.normalize()
  // player.position.x += playerSpeed * playerDirection.x * time.deltaTime
  // player.position.y += playerSpeed * playerDirection.y * time.deltaTime
}

// const playerDirection: Vector2 = new Vector2(0, 0)
// let targetPosition: Vector2 | null = null

const app = new PIXI.Application()

;(globalThis as unknown as { __PIXI_APP__: PIXI.Application }).__PIXI_APP__ =
  app

await app.init({ background: "#8158a3", resizeTo: window })

document.getElementById("pixi-container")!.appendChild(app.canvas)

app.canvas.onclick = (e) => {
  const target = new Vector2(e.clientX - 896, e.clientY - 100)
  const cube = getCubeAt(target)
  if (cube) {
    const direction = Vector2.fromPoint(player.position)
      .sub(Vector2.fromPoint({ x: cube.position.x, y: cube.position.y + 8 }))
      .invert()
    direction.normalize()
    player.goTo(
      Vector2.fromPoint({ x: cube.position.x, y: cube.position.y + 8 }),
    )
    // playerDirection.x = direction.x
    // playerDirection.y = direction.y
    // targetPosition = Vector2.fromPoint({
    //   x: cube.position.x,
    //   y: cube.position.y + 8,
    // })
  }
}

function invertMatrix(a: number, b: number, c: number, d: number) {
  // Determinant
  const det = 1 / (a * d - b * c)

  return {
    a: det * d,
    b: det * -b,
    c: det * -c,
    d: det * a,
  }
}

const i_x = 1
const i_y = 0.5
const j_x = -1
const j_y = 0.5

function getMapCoords(
  target: Vector2,
  offset: Vector2 = new Vector2(0, 0),
): Vector2 {
  const a = i_x * 32
  const b = j_x * 32
  const c = i_y * 32
  const d = j_y * 32

  const inv = invertMatrix(a, b, c, d)

  const adjustedTarget = target.sub(offset)

  const x = adjustedTarget.x * inv.a + adjustedTarget.y * inv.b
  const y = adjustedTarget.x * inv.c + adjustedTarget.y * inv.d

  return new Vector2(x, y)
}

const map: PIXI.Sprite[][] = []
function generateMap() {
  for (let i = 0; i < 20; i++) {
    const row = []
    for (let j = 0; j < 20; j++) {
      const i_hat = (i * i_x + j * j_x) * 32
      const j_hat = (i * i_y + j * j_y) * 32

      const cube = new PIXI.Sprite({
        texture: cubeTexture,
        anchor: { x: 0.5, y: 0 },
        scale: 2,
        position: { x: i_hat + 896, y: j_hat + 100 },
        label: "cube",
      })

      row.push(cube)
      app.stage.addChild(cube)
    }
    map.push(row)
  }
}

PIXI.Assets.add({
  alias: "dwarf",
  src: "/assets/dwarven_scout.png",
  data: {
    scaleMode: "nearest",
  },
})
PIXI.Assets.add({
  alias: "iso-cube",
  src: "/assets/iso-cube.png",
  data: {
    scaleMode: "nearest",
  },
})
const dwarfTexture = await PIXI.Assets.load("dwarf")
const cubeTexture = await PIXI.Assets.load("iso-cube")
const spritesheetFile = await PIXI.Assets.load("public/assets/spritesheet.json")

const spritesheet = new PIXI.Spritesheet(dwarfTexture, spritesheetFile.data)
spritesheet.parse()

// const player = new PIXI.AnimatedSprite(spritesheet.animations["idle_e"])

const player = new PartyMember({
  sprites: {
    idle_e: new PIXI.AnimatedSprite(spritesheet.animations["idle_e"]),
    idle_n: new PIXI.AnimatedSprite(spritesheet.animations["idle_n"]),
    idle_ne: new PIXI.AnimatedSprite(spritesheet.animations["idle_ne"]),
    idle_s: new PIXI.AnimatedSprite(spritesheet.animations["idle_s"]),
    idle_se: new PIXI.AnimatedSprite(spritesheet.animations["idle_se"]),
    idle_w: new PIXI.AnimatedSprite({
      textures: spritesheet.animations["idle_e"],
      scale: { x: -1, y: 1 },
    }),
    run_e: new PIXI.AnimatedSprite(spritesheet.animations["run_e"]),
    run_n: new PIXI.AnimatedSprite(spritesheet.animations["run_n"]),
    run_ne: new PIXI.AnimatedSprite(spritesheet.animations["run_ne"]),
    run_s: new PIXI.AnimatedSprite(spritesheet.animations["run_s"]),
    run_se: new PIXI.AnimatedSprite(spritesheet.animations["run_se"]),
    run_w: new PIXI.AnimatedSprite({
      textures: spritesheet.animations["run_e"],
      scale: { x: -1, y: 1 },
      anchor: { x: 0.5, y: 0.5 },
    }),
  },
})

app.renderer.events.cursorStyles.default =
  "url('/assets/dwarven_gauntlet.png'),auto"

// reset cursor state
app.renderer.events.setCursor("grab")
app.renderer.events.setCursor("default")

generateMap()

// const player = new PIXI.Sprite({
//   texture: dwarfTexture,
//   anchor: { x: 0.5, y: 0.7 },
// })

player.position.set(app.screen.width / 2, app.screen.height / 2)
player.scale.set(2)
player.zIndex = 10

app.stage.addChild(player)

app.ticker.add(update)

const graphics = new PIXI.Graphics()
  // .rect(0, 0, 64, 64)
  .regularPoly(0, 0, 32, 4, Math.PI / 2)
  .fill(0xffffff)
  .moveTo(0, 0)

graphics.height = 32
graphics.zIndex = 5

const text = new PIXI.Text({
  text: "x: " + 0 + " y: " + 0,
  style: {
    fontFamily: "monospace",
    fontSize: 32,
    fill: 0xffffff,
  },
})
app.stage.addChild(text)

app.stage.addChild(graphics)

function getCubeAt(position: Vector2, offset: Vector2 = new Vector2(0, 0)) {
  const mapCoords = getMapCoords(position, offset)

  const y = Math.floor(mapCoords.y)
  const x = Math.floor(mapCoords.x)
  text.text = `real: ${mapCoords} 
xy: ${x}, ${y}`
  // "x: " + x + " y: " + y + "\nmapCoords: " + mapCoords.x + " " + mapCoords.y
  if (x < 0 || x >= map.length || y < 0 || y >= map[0].length) {
    return null
  }
  return map[x][y]
}

app.canvas.onmousemove = (e) => {
  const cube = getCubeAt(
    new Vector2(e.clientX, e.clientY),
    new Vector2(896, 100),
  )
  if (cube) {
    graphics.alpha = 0.25
    graphics.position.set(cube.position.x, cube.position.y + 16)
  } else {
    graphics.alpha = 0
  }
}
